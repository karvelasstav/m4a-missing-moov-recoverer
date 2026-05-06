# .m4a Missing `moov` Recoverer

have you found yourself with a voice memo on your phone ending in `.m4a` that was improperly saved and now won't open even though you can see it is large? have you used a hex editor only to find that you are missing a `moov` at the end? have you looked everywhere but nothing seems to actually work? well i was in the same boat and have made this little tool.

this is a cli program that takes a broken m4a file which is broken because it is missing the `moov` part (basically the recording wasn't stopped correctly and now you have the stream of compressed data but not the metadata needed to actually open it). **THIS PROGRAM IS NOT FOR GENERAL CORRUPTION, IT IS FOR PREMATURE TERMINATION** (so basically if you recorded it and it could never open AND the file has actual size it probably was terminated prematurely by your application). to actually verify if that's your problem you can use a hex editor to inspect if you can find a `moov` tag although you do not have to if you don't know how to.

## Requirements:
this program requires the program FAAD2 to work, you can find it on github [here](https://github.com/knik0/faad2), this program actually does the heavy lifting. you are also advised to use ffmpeg because otherwise you will have as sole output a wav file that can be quite large

## How to Use:
since i know most people that stumble on this tool are probably not that well acquainted with the intricacies of the m4a format and the manipulation of single bytes and many of them may never have even used a CLI program before this is a very very simple guide to get the program to work:

1) go to builds and download the latest build, unzip it and get the executable

2) go to https://github.com/knik0/faad2 and install a build from there too, get the executable and place it in the same folder as the previous executable (ideally you would do `--faad <path>` as a flag but if you don't know what that means don't worry about it)

3) open command prompt, type `cd C:\path\to\the\executable\folder` and enter, you should now see on the left the path you just entered

4) go to where your `.m4a` is stored and copy the path to the file including the file (if you hold shift while right clicking you can directly select "copy path")

5) now in your command prompt write `m4a-missing-moov-recoverer.exe --sr 48000 --encode-m4a "your/path/goes/here/file.m4a"` ideally you would also download ffmpeg and then use `--encode-m4a` and also potentially `--ffmpeg` to get an actual m4a at the end of this process, if you don't have them installed yet don't worry you can just ignore this and see if you even get a wav

6) press enter and wait, you should see a lot of checks going quickly by your screen, at some point it should stop and ask you if it should investigate (this means that it managed to create an audio file from the first 64kb which means that most probably you found the correct reading frame), you should answer by pressing `y` and pressing Enter unless you have already tried the specific spot before without success

7) the program will do its magic and in the end in the folder where the broken `.m4a` is located should be a `recovered_broken.wav` (and even a `recovered_broken.m4a` if you had set up ffmpeg in the previous step, it might take a couple of minutes to go from the wav to the m4a though). open it and see if you are satisfied


## Building:
this is a single file C++ application that uses only STL headers and the c++ 17 standard. just set the standard to c++ 17 and compile it in a compiler of your choice or use visual studio to open the solution file directly from the github repo
