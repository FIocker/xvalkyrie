# Third party

## xbdepends tables (Microsoft, MIT)

`src/compatibility/xbdepends_data.hpp` is generated from the DLL and API name
tables of the `xbdepends` sample in
[microsoft/Xbox-GDK-Samples](https://github.com/microsoft/Xbox-GDK-Samples)
(commit `b1e77b6ac10f95b07296025fda171ca2ddc42dbe`). The file name patterns in
`src/compatibility/modules.cpp` come from the same tool. That sample is MIT
licensed and the license asks for this notice to be kept:

    MIT License

    Copyright (c) Microsoft Corporation.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE

## Libraries

Fetched by CMake at configure time, each under its own license: GLFW,
Dear ImGui, Native File Dialog Extended, stb_image (logo decoding), spdlog, fmt,
nlohmann/json, pugixml and Catch2.
`stb_image` is fetched from [nothings/stb](https://github.com/nothings/stb)
at commit `5736b15f7ea0ffb08dd38af21067c314d6a3aae9` (MIT/public domain).
