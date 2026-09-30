## **Requirements**

- WebView2 Runtime (should be preinstalled on modern Windows Versions)
- An existing pdflatex installation added to PATH

## **Installation**

Go to the latest version under ``releases`` and download the latest ```NppLatexPreview.dll``` and ```WebView2Loader.dll```. Move them into Notepad++'s plugin folder under ```NppLatexPreview```, e.g. ```C:\Program Files\Notepad++\plugins\NppLatexPreview```.

## **To get plugin DLLs by building manually:**

1. Clone locally and open cmd in the relevant directory, e.g.:

```
git clone https://github.com/cjcub3/npp-latex-preview.git C:\path\to\destination\folder\directory
cd C:\path\to\destination\folder\directory
```

2. Run CMake (install if necessary):

```
rmdir /s /q build
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

3. ```NppLatexPreview.dll``` and ```WebView2Loader.dll``` will be generated in build/plugin. Move them into Notepad++'s plugin folder under ```NppLatexPreview```, e.g. ```C:\Program Files\Notepad++\plugins\NppLatexPreview```

## **Basic Usage**

1. Navigate to a ```.tex``` file and in the menu under ```NppLatexPreview``` click "Show LaTeX Preview" to create preview panel.
2. Click "Compile" in panel header to compile. If it fails, relevant errors will be shown in the header.
3. Under "Settings", there are currently 3 options: compile on save, synctex on compile and show compile errors as message boxes. By default, only the first is enabled.
    - **Compile on save**: Enabled by default. Whenever the current ```.tex``` file is saved (e.g. through Ctrl+S or menu commands), the file attempts to recompile. If it succeeds, the new PDF shows; if it fails, the old PDF remains.
    - **SyncTeX on compile**: Disabled by default. SyncTeX is a tool to jump between lines in the ```.tex``` source file and positions in the compiled PDF produced by said line. If this setting is enabled, using Ctrl+Click on any section of the PDF viewer should cause the cursor to jump to the source file and line (which contains the LaTeX source code responsible for the part of the PDF near the clicked position).
    - **Show compile errors as message boxes**: Disabled by default. If enabled, in addition to showing information on the failed LaTeX compilation in the header, a message box will also appear with more detailed information.
5. "Perform SyncTeX Forward Search" also exists as a plugin command. This makes it bindable in a keyboard shortcut or usable directly if one would like to jump to the page corresponding to the cursor line without compiling or enabling the setting.

Note: Current SyncTeX implementation is unstable. If the setting is enabled but Ctrl+Click does not register, try recompiling or restarting Notepad++. Behaviour for multi-file compilations, such as through ```\include``` and ```\input``` is untested.
