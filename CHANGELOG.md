# Changelog

## [0.3.0](https://github.com/joberthel/openlnk/compare/v0.2.0...v0.3.0) (2026-11-27)


### Features

* **esp32:** add thread-safe client wrapper and FatFs VFS /dos mount ([cad1cdc](https://github.com/joberthel/openlnk/commit/cad1cdcdc0e83cf0783ac6e6dac1196b0c183d9e))
* **esp32:** complete production-ready ESP32 HAL, examples, tests, and release bundle ([ac9f270](https://github.com/joberthel/openlnk/commit/ac9f270b226829a8b126a25149973a09f33daca1))


### Bug Fixes

* **dos:** support RAMDRIVE.SYS / non-bootable BPBs via INT 21h 440Dh and relaxed VBR ([5fa7cfb](https://github.com/joberthel/openlnk/commit/5fa7cfb2184e8884dd0a34f509678b0c6efbb58a))
* **link:** handle seq==0 high-baud reconnect, client retry spacing & DOS UART FE fast-drop ([7c26bc6](https://github.com/joberthel/openlnk/commit/7c26bc6cfbda898c65ee9e8517e1e36419406eb8))
* **openlnk:** clear POSIX CRTSCTS and IGNPAR for 3-wire USB-serial links ([4eeaa86](https://github.com/joberthel/openlnk/commit/4eeaa8632e398a88c61e82c9b7366e3537e40536))
* **opensvr:** raise default idle high-baud timeout to 60s and add --idle-timeout ([cca2ce6](https://github.com/joberthel/openlnk/commit/cca2ce68377464c7c3499f5907d19b92754ab288))
* **server:** account for real RS-232 burst TX wire time and 9600 bps resync ([31c3023](https://github.com/joberthel/openlnk/commit/31c3023c2c4a19b6a4ef130ab5b2ef26df5dbc6e))
* **server:** recover immediately from 9600 bps probes while in high-baud RX drain ([2e41961](https://github.com/joberthel/openlnk/commit/2e4196131d12822404cba98364d7c0dc84186cb4))

## [0.2.0](https://github.com/joberthel/openlnk/compare/v0.1.0...v0.2.0) (2026-11-27)


### Features

* **analysis:** add 100% golden trace verifier, CRC-16/GENIBUS, and fixture extractor ([e803694](https://github.com/joberthel/openlnk/commit/e80369465a96dcbd8b6a772b0661ff8ad73c1753))
* **cleanroom:** implement zero-alloc C99 libinterlnk core, DOS and ESP32 targets ([32cfc8b](https://github.com/joberthel/openlnk/commit/32cfc8b357046d4a8640abf1af93e892de476cd1))
* **dos:** implement loadable 16-bit Real-Mode OPENLNK.SYS block device driver ([aeb4be6](https://github.com/joberthel/openlnk/commit/aeb4be63cc53b48a80e0be6e18f6979a4cde60fb))
* **libopenlnk:** implement verified MS-DOS 6.22 wire protocol, link engine, FAT storage, and server ([6b2f8c9](https://github.com/joberthel/openlnk/commit/6b2f8c9cd94868f7407f92b1d21f35e58d4b3af9))
* **libopenlnk:** implement zero-alloc MS-DOS 6.22 client RPC engine (olnk_client_t) ([257d3e0](https://github.com/joberthel/openlnk/commit/257d3e0a18aee6e6f4f757efae4a7ef6d2a97f95))
* **openlnk:** add full POSIX/86Box client CLI (info/ls/cat/get/dump/write/shell) and client tests ([3caa1d5](https://github.com/joberthel/openlnk/commit/3caa1d51776a331e1fb97e0a5e22d331f222f0e9))
* **opensvr:** add POSIX serial/86Box pipe server, VHD/MBR storage, 24/24 vector-check, and DOS targets ([f5b41f8](https://github.com/joberthel/openlnk/commit/f5b41f863d35c51450c7391799b9a86d5d56a1ee))
* **opensvr:** add Read-Write Virtual FAT16 directory sharing (--dir) with live host refresh ([018a1c2](https://github.com/joberthel/openlnk/commit/018a1c2af08510525b9857ecfc466dc695483d57))
* **runner:** add --machine 486 profile (486sp3g / NS16550A 16-byte FIFO UART) ([756c1da](https://github.com/joberthel/openlnk/commit/756c1da438c84e094dcdc5ea6fd0b6eeedeb1096))
* **runner:** support 3-disk MS-DOS 6.22 images and KWAJ FINSETUP staging ([1a3a470](https://github.com/joberthel/openlnk/commit/1a3a47002c2eb059132877440dd54b2684e9209f))
* **sniffer:** add --pace-us byte pacing for 86Box Named Pipe 8250 UART stability ([e892050](https://github.com/joberthel/openlnk/commit/e89205020cd32294335dd3bd21a6740d07038643))
* **sniffer:** add 86Box Named Pipe (COM) FIFO relay mode for BIOS INT 14h / CTTY ([77cd90b](https://github.com/joberthel/openlnk/commit/77cd90b180f27ea7c6dae312287b3c3507c55966))
* **spec:** publish verified MS-DOS 6.22 INTERLNK/INTERSVR Kaitai spec and protocol doc ([4f283b7](https://github.com/joberthel/openlnk/commit/4f283b714cbba10cc9275409438c941ed7824fd1))


### Bug Fixes

* **cleanroom:** resolve Ubuntu GCC -Werror and Windows MSVC C4701 build errors ([cdd954e](https://github.com/joberthel/openlnk/commit/cdd954e74d6656cabf11f46201b3c83922a0e908))
* **cmake:** namespace library alias openlnk::openlnk to avoid clash with openlnk CLI ([679e0c7](https://github.com/joberthel/openlnk/commit/679e0c760cf2950013145eb4117c2082d5159e25))
* **runner:** enable ide_isa storage controller on 486sp3g profile ([ed50e02](https://github.com/joberthel/openlnk/commit/ed50e02f44e7a89a66a386e2e35635683044726d))
* **runner:** sanitize dos622_boot.img CONFIG.SYS for XT (remove CD-ROM/HIMEM drivers) ([f5a64c6](https://github.com/joberthel/openlnk/commit/f5a64c67e3560a9ad661534eb4f0c576973f01d7))
* **runner:** switch 486 profile to 486vipio2 (onboard IDE + 16550A, no SCSI, interpreter) ([9dce78f](https://github.com/joberthel/openlnk/commit/9dce78f63f7b1700503b78fa122d5ef83930ca8d))
* **runner:** use /COM1 /BAUD:&lt;rate&gt; syntax and add --finalize-from-client VHD cloner ([39bf7fe](https://github.com/joberthel/openlnk/commit/39bf7fe6014c0cfdbcf3500283ce60e62652c734))
* **runner:** use 86Box build 9001 [Serial Passthrough (COM) [#1](https://github.com/joberthel/openlnk/issues/1)] and path= key ([3a0d11b](https://github.com/joberthel/openlnk/commit/3a0d11b5efba7239b0c4964ffeeb5dc572f95718))
* **runner:** use matching xta_wdxt150 controller on 486 profile to match VHD BPB geometry ([56c8170](https://github.com/joberthel/openlnk/commit/56c8170f08b9d0c838d21e7b483556fd08fb2a30))
* **runner:** use native 86Box v4/v5 INI keys (gfxcard, mem_size, genxt, dtk_pii151b) ([29003aa](https://github.com/joberthel/openlnk/commit/29003aad7e354269db0ec38e35ae62aca4157612))
* **sniffer:** match 86Box Named Pipe FIFO direction (read .out, write .in) ([2bcbb02](https://github.com/joberthel/openlnk/commit/2bcbb0242b267633ccc02dafa9ba43dffe2eb1f6))
* **sniffer:** persist pacing timestamp across consecutive 1-byte FIFO reads ([dc84815](https://github.com/joberthel/openlnk/commit/dc848153e583b0f678625a094817c77828dfe0a5))
* **sniffer:** reuse existing FIFOs in-place so restarting sniffer keeps 86Box connected ([e1d04d2](https://github.com/joberthel/openlnk/commit/e1d04d240c83a43d98ae990abf49fabf2b775255))
* **sniffer:** use accurate perf_counter_ns byte pacing (avoid macOS sleep coalescing) ([4aaab7f](https://github.com/joberthel/openlnk/commit/4aaab7fe7878e63919f17f40b912dcb78b7e704c))
* **watcom:** use ow-snapshot.tar.xz URL and record 16-bit DOS MZ build verification ([f5f27d6](https://github.com/joberthel/openlnk/commit/f5f27d672927125e1ea3b5eaafd24f6b9a653422))


### Performance Improvements

* **dos:** minimize 16-bit DOS disk & RAM footprint with 100% Zero-CRT ([cbd9ab9](https://github.com/joberthel/openlnk/commit/cbd9ab985f9090343a45a65334aa629dacd3f226))
