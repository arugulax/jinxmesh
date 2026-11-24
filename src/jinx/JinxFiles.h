#pragma once

#include <stdint.h>

/**
 * Files for the card over the serial console (the USB cable), for boards whose card cannot be taken out
 * and where Wi-Fi is no option. The other end is kbview-harness/webflash/webflash ("put", "ls", ...); the
 * protocol is described in JinxFiles.cpp.
 */

/**
 * Takes every byte of the console that is not part of a client API frame. True when that byte completed
 * the request to start, and the serial port now belongs to the file transfer.
 */
bool jinxFilesByte(uint8_t c);

/**
 * True while the file transfer owns the serial port: the console must neither read nor answer.
 */
bool jinxFilesActive(void);
