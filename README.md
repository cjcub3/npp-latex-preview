Current State: WIP, Source code available, release workflows not done yet (workflows in .github are template Visual Studio compilers from the npp plugin template repo). Repo is also very unclean, e.g. cloned_src and some .txts are temporary working files that aren't in .gitignore

## **Requirements**

Cmake and WebView2 Runtime are necessary for this plugin to function.

## **To get plugin DLLs now:**

1. Clone locally and open cmd in the relevant directory

2. Run CMake (install if necessary):

```
rmdir /s /q build
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

3. ```NppLatexPreview.dll``` and ```WebView2Loader.dll``` will be generated in build/plugin. Move them into Notepad++'s plugin folder under ```NppLatexPreview```, e.g. ```C:\Program Files\Notepad++\plugins\NppLatexPreview```

4. Under the existing MinGW installation binary folder (e.g. ```C:\mingw64\bin```) find ```libwinpthread-1.dll``` and copy into the plugin folder as well. This dependency has yet to be removed so this is necessary if you are reading this now.

## **Basic Usage**

1. Navigate to a ```.tex``` file and in the menu under ```NppLatexPreview``` click "Show LaTeX Preview" to create preview panel.
2. Click "Compile" in panel header to compile. If it fails, relevant errors will be shown in the header.
3. Under settings, there are currently 3 options, which are self-explanatory. These are persistent.
4. "Perform SyncTeX Forward Search" also exists as a plugin command which is bindable to a keyboard shortcut if one would like to jump to the page corresponding to the cursor line without compiling.
