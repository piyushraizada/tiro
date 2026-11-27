THIRD-PARTY COMPONENTS IN THIS DIRECTORY
========================================

The build links two third-party C libraries. Their C API headers and their
licences live here, committed, so that a clone can be read, reviewed and
licence-audited without downloading anything.

  include\sherpa-onnx-c-api.h
      sherpa-onnx v2.0.3 C API
      Apache License 2.0 - see licenses\sherpa-onnx-LICENSE.txt
      https://github.com/k2-fsa/sherpa-onnx

  include\onnxruntime_c_api.h
      ONNX Runtime C API, ORT_API_VERSION 20 (shipped by ONNX Runtime 1.20.1)
      MIT License - see licenses\onnxruntime-LICENSE.txt
      https://github.com/microsoft/onnxruntime

  sherpa-onnx-v2.0.3-win-x64-static-MT-Release-no-tts-lib\lib\*.lib
      14 static libraries, taken unmodified from the official sherpa-onnx
      v2.0.3 release asset of the same name. Committed as
      sherpa-onnx-v2.0.3-win-x64-static-MT-Release-no-tts-lib.tar.xz and
      unpacked into this folder by build_release.bat.
      sherpa-onnx is Apache-2.0; the package embeds kaldi, OpenFst, kissfft,
      utf8proc, sentencepiece and ONNX Runtime, all permissively licensed.
      The list shipped with the binary is dist\NOTICE.txt, and the full
      licence texts are in dist\LICENSE.txt.

Licence position on redistribution
----------------------------------
Apache-2.0 (sherpa-onnx) and MIT (ONNX Runtime) both permit redistribution of
source, headers and compiled binaries in commercial or open products, provided
the copyright and permission notices travel with the material and, for
Apache-2.0, any NOTICE file is preserved. sherpa-onnx v2.0.3 ships no NOTICE
file. That is what this folder does: the headers are committed next to their
licence texts, and the shipped binary carries the attribution in
dist\LICENSE.txt and dist\NOTICE.txt.

Why the libraries are still a download
--------------------------------------
onnxruntime.lib alone is 944 MB and the 14 libraries together are 1.1 GB.
GitHub rejects any single file over 100 MB, so the archives cannot be
committed even though the licence allows it - vendoring them into Git LFS
would also exceed the free 1 GB storage quota and make every clone download a
gigabyte. The two headers together are 400 KB, so they are committed and
BUILD.md asks for exactly one download instead of three steps.
