# Third-party notes

## fabioferrero90/webcam-settings (MIT)

The IOKit/USB technique in `uvc_core.c` — enumerating UVC devices via
`IOServiceGetMatchingServices`, parsing the Video Control descriptor tree to
locate the camera terminal and processing unit, and issuing class-specific
`GET_CUR`/`SET_CUR` requests with `IOUSBDevRequest` — is adapted from
[webcam-settings](https://github.com/fabioferrero90/webcam-settings)
by Fabio Ferrero, MIT licensed. Thank you, Fabio — the compact reference
implementation proved the approach still works on modern macOS and saved
a lot of trial and error.

Its license text, required to accompany adapted portions:

```
MIT License

Copyright (c) 2026 Fabio Ferrero

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
SOFTWARE.
```

Everything else in this repository (the exposure/WB gating reverse engineering,
the `streamcam-ctl` CLI, the highlight-protective auto-exposure loop, the lab
tools) is original work.
