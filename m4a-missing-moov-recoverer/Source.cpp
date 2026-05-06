#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Options {

    fs::path inputPath;
    fs::path faadPath;
    std::size_t maxStarts = std::numeric_limits<std::size_t>::max();
    std::size_t probeSize = 65536;
    int sampleRate = 48000;
    bool rawMode = false;
    bool debug = false;
    bool encodeM4a = false;
    fs::path ffmpegPath;
};

std::string asciiPreview(const std::vector<char>& data, std::size_t offset) {
    std::string out;

    for (std::size_t i = offset; i < offset + 20 && i < data.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        out += (c >= 32 && c <= 126) ? static_cast<char>(c) : '*';
    }

    return out;
}

std::uint32_t readBe32(const std::vector<char>& data, std::size_t pos) {
    return
        (static_cast<unsigned char>(data[pos]) << 24) |
        (static_cast<unsigned char>(data[pos + 1]) << 16) |
        (static_cast<unsigned char>(data[pos + 2]) << 8) |
        static_cast<unsigned char>(data[pos + 3]);
}

std::optional<std::size_t> findMdatPayloadStart(const std::vector<char>& data) {
    for (std::size_t i = 4; i + 4 <= data.size(); ++i) {
        if (data[i] == 'm' && data[i + 1] == 'd' && data[i + 2] == 'a' && data[i + 3] == 't') {
            std::uint32_t boxSize = readBe32(data, i - 4);

            if (boxSize == 1 && i + 12 <= data.size()) {
                return i + 12;
            }

            return i + 4;
        }
    }

    return std::nullopt;
}

fs::path executableDir() {
    char buffer[MAX_PATH]{};
    DWORD len = GetModuleFileNameA(nullptr, buffer, MAX_PATH);

    if (len == 0 || len == MAX_PATH) {
        return fs::current_path();
    }

    return fs::absolute(fs::path(buffer)).parent_path();
}

std::size_t parseSize(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), ::isspace), s.end());

    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (s == "inf" || s == "infinite") {
        return std::numeric_limits<std::size_t>::max();
    }

    std::size_t mult = 1;

    if (!s.empty()) {
        char suffix = s.back();

        if (suffix == 'k') {
            mult = 1024;
            s.pop_back();
        }
        else if (suffix == 'm') {
            mult = 1024ULL * 1024ULL;
            s.pop_back();
        }
        else if (suffix == 'g') {
            mult = 1024ULL * 1024ULL * 1024ULL;
            s.pop_back();
        }
    }

    return static_cast<std::size_t>(std::stoull(s) * mult);
}

std::string quoteArg(const std::string& s) {
    std::string out = "\"";

    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }

    out += "\"";
    return out;
}

bool runProcessCapture(
    const fs::path& exe,
    const std::vector<std::string>& args,
    std::string& output,
    DWORD& exitCode
) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;

    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        return false;
    }

    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::string exeStr = exe.string();
    std::string cmdLine = quoteArg(exeStr);

    for (const std::string& arg : args) {
        cmdLine += " ";
        cmdLine += quoteArg(arg);
    }

    std::vector<char> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessA(
        nullptr,
        mutableCmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    CloseHandle(writePipe);

    if (!ok) {
        CloseHandle(readPipe);
        return false;
    }

    output.clear();

    char buffer[4096];
    DWORD bytesRead = 0;

    while (ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        output.append(buffer, buffer + bytesRead);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);

    return true;
}


bool encodeToM4a(
    const fs::path& ffmpeg,
    const fs::path& wav,
    const fs::path& outM4a
) {

    DWORD exitCode = 0;
    std::string output;

    return runProcessCapture(
        ffmpeg,
        {
            "-y",
            "-i",
            wav.string(),
            "-c:a",
            "aac",
            "-b:a",
            "192k",
            outM4a.string()
        },
        output,
        exitCode
    ) && fs::exists(outM4a);
}


bool wavLooksValid(const fs::path& wavPath) {
    std::error_code ec;

    if (!fs::exists(wavPath, ec)) {
        return false;
    }

    auto size = fs::file_size(wavPath, ec);

    return !ec && size > 4096;
}

bool writeSlice(
    const std::vector<char>& data,
    std::size_t offset,
    std::size_t size,
    const fs::path& outPath
) {
    if (offset >= data.size()) {
        return false;
    }

    size = std::min<std::size_t>(size, data.size() - offset);

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);

    if (!out) {
        return false;
    }

    out.write(data.data() + offset, size);
    return static_cast<bool>(out);
}

bool runFaad(
    const Options& opt,
    const fs::path& rawPath,
    const fs::path& wavPath,
    std::string& output
) {
    std::error_code ec;
    fs::remove(wavPath, ec);

    DWORD exitCode = 0;

    bool launched = runProcessCapture(
        opt.faadPath,
        {
            "-l", "2",
            "-s", std::to_string(opt.sampleRate),
            "-o", wavPath.string(),
            rawPath.string()
        },
        output,
        exitCode
    );

    return launched && wavLooksValid(wavPath);
}

void saveText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

void investigateLargestWindow(
    const Options& opt,
    const std::vector<char>& data,
    std::size_t offset,
    const fs::path& outDir
) {
    std::cout << "\nInvestigate largest working size? (y/n): ";

    char response = 'n';
    std::cin >> response;

    if (response != 'y' && response != 'Y') {
        return;
    }

    const std::size_t remaining = data.size() - offset;
    const std::size_t knownGood = std::min<std::size_t>(opt.probeSize, remaining);

    fs::path testRaw = outDir / "investigate.raw";
    fs::path testWav = outDir / "investigate.wav";

    std::string bestOutput;
    std::size_t best = knownGood;

    auto testSize = [&](std::size_t size, std::string& output) {
        writeSlice(data, offset, size, testRaw);
        return runFaad(opt, testRaw, testWav, output);
        };

    std::string output;

    std::cout << "Testing size " << remaining << " bytes -> ";

    if (testSize(remaining, output)) {
        std::cout << "SUCCESS\n";
        best = remaining;
        bestOutput = output;
    }
    else {
        std::cout << "FAIL\n";

        std::size_t highFail = remaining;
        std::size_t current = remaining / 2;

        while (current > best) {
            std::cout << "Testing size " << current << " bytes -> ";

            if (testSize(current, output)) {
                std::cout << "SUCCESS\n";
                best = current;
                bestOutput = output;
                break;
            }

            std::cout << "FAIL\n";
            highFail = current;
            current /= 2;
        }

        std::size_t low = best + 1;
        std::size_t high = highFail > 0 ? highFail - 1 : 0;

        while (low <= high) {
            std::size_t mid = low + (high - low) / 2;

            std::cout << "Testing size " << mid << " bytes -> ";

            if (testSize(mid, output)) {
                std::cout << "SUCCESS\n";
                best = mid;
                bestOutput = output;
                low = mid + 1;
            }
            else {
                std::cout << "FAIL\n";

                if (mid == 0) break;
                high = mid - 1;
            }
        }
    }

    std::string stem =
        opt.inputPath.stem().string();

    fs::path bestRaw =
        outDir /
        ("recovered_" + stem + ".raw");

    fs::path bestWav =
        outDir /
        ("recovered_" + stem + ".wav");

    fs::path bestLog =
        outDir /
        ("recovered_" + stem + ".log");

    fs::path bestM4a =
        outDir /
        ("recovered_" + stem + ".m4a");

    writeSlice(data, offset, best, bestRaw);
    runFaad(opt, bestRaw, bestWav, bestOutput);
    if (opt.debug) {
        saveText(bestLog, bestOutput);
    }
    else {
        std::error_code ec;
        fs::remove(bestRaw, ec);
    }
    if (opt.encodeM4a) {

        std::cout
            << "Encoding AAC m4a...\n";

        if (encodeToM4a(
            opt.ffmpegPath,
            bestWav,
            bestM4a
        )) {

            std::cout
                << "Saved m4a: "
                << bestM4a
                << "\n";
        }
        else {

            std::cout
                << "Failed to encode m4a\n";
        }
    }

    std::cout << "\nLargest working size: " << best << " bytes\n";
    

    std::cout << "Saved wav: " << bestWav << "\n";
    if (!opt.debug) {

        std::error_code ec;

        fs::remove(testRaw, ec);
        fs::remove(testWav, ec);
    }

    if (opt.debug) {
        std::cout << "Saved log: " << bestLog << "\n";
        std::cout << "Saved raw: " << bestRaw << "\n";
    }
}

void printHelp(const char* exeName) {
    std::cout
        << "Usage:\n"
        << "  " << exeName << " [options] <file.m4a|raw>\n\n"
        << "Options:\n"
        << "  -h, --help              Show this help\n"
        << "  --faad <path>           Path to faad.exe. Default: faad.exe beside this tool\n"
        << "  --max-size <N>          Max candidate start offsets to test. Supports K/M/G/inf\n"
        << "  --probe-size <N>        Bytes tested per offset. Default: 64K\n"
        << "  --sample-rate <N>     AAC sample rate. Default: 48000\n"
        << "  --raw                   Do not auto-skip to mdat payload\n\n"
        << "  --encode-m4a          Encode recovered wav to AAC m4a using ffmpeg\n"
        << "  --ffmpeg <path>       Path to ffmpeg.exe in order to return a compressed m4a instead of a wav\n"
        << "  --debug               Keep logs/probes/intermediate files\n"
        << "Examples:\n"
        << "  recover.exe E:\\MYAudio\\m4a\\broken.m4a\n"
        << "  recover.exe --faad E:\\faad2\\bin\\faad.exe --max-size 100000 E:\\MYAudio\\m4a\\broken.m4a\n";
}

bool parseArgs(int argc, char** argv, Options& opt) {
    opt.faadPath = executableDir() / "faad.exe";
    opt.ffmpegPath = executableDir() / "ffmpeg.exe";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];

        if (a == "-h" || a == "--help") {
            printHelp(argv[0]);
            return false;
        }

        if (a == "--faad") {
            if (++i >= argc) throw std::runtime_error("--faad requires a path");
            opt.faadPath = argv[i];
            continue;
        }

        if (a == "--max-size" || a == "--max-offset") {
            if (++i >= argc) throw std::runtime_error("--max-size requires a value");
            opt.maxStarts = parseSize(argv[i]);
            continue;
        }

        if (a == "--probe-size") {
            if (++i >= argc) throw std::runtime_error("--probe-size requires a value");
            opt.probeSize = parseSize(argv[i]);
            continue;
        }
        if (a == "--sample-rate" || a == "--sr") {
            if (++i >= argc)
                throw std::runtime_error("--sample-rate requires a value");

            opt.sampleRate = std::stoi(argv[i]);
            continue;
        }
        if (a == "--raw") {
            opt.rawMode = true;
            continue;
        }
        if (a == "--debug") {
            opt.debug = true;
            continue;
        }

        if (a == "--encode-m4a") {
            opt.encodeM4a = true;
            continue;
        }

        if (a == "--ffmpeg") {
            if (++i >= argc)
                throw std::runtime_error("--ffmpeg requires a path");

            opt.ffmpegPath = argv[i];
            continue;
        }

        if (!opt.inputPath.empty()) {
            throw std::runtime_error("Multiple input files supplied");
        }

        opt.inputPath = argv[i];
    }

    if (opt.inputPath.empty()) {
        printHelp(argv[0]);
        return false;
    }

    return true;
}

int main(int argc, char** argv) {
    Options opt;

    try {
        if (!parseArgs(argc, argv, opt)) {
            return 0;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << "\n\n";
        printHelp(argv[0]);
        return 1;
    }

    if (!fs::exists(opt.inputPath)) {
        std::cerr << "Input file not found: " << opt.inputPath << "\n";
        return 1;
    }

    if (!fs::exists(opt.faadPath)) {
        std::cerr << "faad.exe not found: " << opt.faadPath << "\n";
        return 1;
    }
    if (opt.encodeM4a) {

        DWORD attrs = GetFileAttributesA(
            opt.ffmpegPath.string().c_str()
        );

        bool existsDirect =
            attrs != INVALID_FILE_ATTRIBUTES;

        if (!existsDirect) {

            DWORD exitCode = 0;
            std::string dummy;

            bool foundInPath = runProcessCapture(
                "ffmpeg.exe",
                { "-version" },
                dummy,
                exitCode
            );

            if (foundInPath) {
                opt.ffmpegPath = "ffmpeg";
            }
            else {
                std::cerr
                    << "ffmpeg.exe not found\n";

                return 1;
            }
        }
    }

    std::ifstream in(opt.inputPath, std::ios::binary);

    if (!in) {
        std::cerr << "Cannot open input file\n";
        return 1;
    }

    std::vector<char> data(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>()
    );

    if (data.empty()) {
        std::cerr << "Input file is empty\n";
        return 1;
    }

    fs::path outDir = opt.inputPath.parent_path();

    if (outDir.empty()) {
        outDir = fs::current_path();
    }

    std::size_t baseOffset = 0;

    if (!opt.rawMode) {
        auto mdatStart = findMdatPayloadStart(data);

        if (mdatStart) {
            baseOffset = *mdatStart;
            std::cout << "mdat payload starts at file offset " << baseOffset << "\n";
        }
        else {
            std::cout << "No mdat found; searching from file offset 0\n";
        }
    }

    if (baseOffset >= data.size()) {
        std::cerr << "Search start is beyond end of file\n";
        return 1;
    }

    std::size_t availableStarts = data.size() - baseOffset;
    std::size_t maxStarts = std::min(opt.maxStarts, availableStarts);

    fs::path probeRaw = outDir / "aac_probe.raw";
    fs::path probeWav = outDir / "aac_probe.wav";

    std::cout << "Input: " << opt.inputPath << "\n";
    std::cout << "FAAD: " << opt.faadPath << "\n";
    std::cout << "Testing " << maxStarts << " candidate offset(s)\n";

    for (std::size_t rel = 0; rel < maxStarts; ++rel) {
        std::size_t offset = baseOffset + rel;

        std::cout
            << "offset " << offset
            << " rel " << rel
            << " \"" << asciiPreview(data, offset) << "\"\n";

        writeSlice(data, offset, opt.probeSize, probeRaw);

        std::string output;
        bool ok = runFaad(opt, probeRaw, probeWav, output);

        if (!ok) {
            continue;
        }
        std::error_code ec;

        fs::path hitRaw;
        fs::path hitWav;
        fs::path hitLog;

        if (opt.debug) {

            hitRaw =
                outDir /
                ("hit_offset_" + std::to_string(offset) + "_probe.raw");

            hitWav =
                outDir /
                ("hit_offset_" + std::to_string(offset) + "_probe.wav");

            hitLog =
                outDir /
                ("hit_offset_" + std::to_string(offset) + ".log");

            fs::copy_file(
                probeRaw,
                hitRaw,
                fs::copy_options::overwrite_existing,
                ec
            );

            fs::copy_file(
                probeWav,
                hitWav,
                fs::copy_options::overwrite_existing,
                ec
            );

            saveText(hitLog, output);
        }

        std::cout << "\n====================================\n";
        std::cout << "VALID WAV CREATED\n";
        std::cout << "File offset: " << offset << "\n";
        std::cout << "Relative offset: " << rel << "\n";
        std::cout << "First 20 bytes ASCII: \"" << asciiPreview(data, offset) << "\"\n";

        if (opt.debug) {

            std::cout << "Saved raw: " << hitRaw << "\n";
            std::cout << "Saved wav: " << hitWav << "\n";
            std::cout << "Saved log: " << hitLog << "\n";
        }
        else {

            std::error_code cleanupEc;

            fs::remove(probeRaw, cleanupEc);
            fs::remove(probeWav, cleanupEc);
        }
        investigateLargestWindow(opt, data, offset, outDir);
        return 0;
    }

    std::error_code ec;
    fs::remove(probeRaw, ec);
    fs::remove(probeWav, ec);

    std::cout << "\nNo valid WAV found.\n";
    return 0;
}