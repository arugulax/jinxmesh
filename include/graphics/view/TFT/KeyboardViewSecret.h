#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * Secrets of the keyboard view: random numbers that deserve the name and the sealing of what
 * the preferences file has to hold but must not show.
 */
namespace kbview
{

/** Bytes from the hardware random number generator, from the system's one on the simulator. */
void randomBytes(uint8_t *bytes, size_t size);

/**
 * Seals a secret of up to 64 bytes for the preferences file: ChaCha20-Poly1305 under a key
 * this device made for itself and keeps outside the file system (NVS on the ESP32). The
 * result is one line of text that tells neither the secret nor its length. Empty when the
 * secret is too long or the key cannot be had.
 *
 * The file alone is useless to a reader. Somebody who can read the whole flash chip gets the
 * key as well: only flash encryption helps against that.
 */
std::string sealSecret(const std::string &secret);

/** The reverse. False when the text was not sealed by this device or is damaged. */
bool openSecret(const std::string &sealed, std::string &secret);

/**
 * Builds before 2026-11-19 let the Wi-Fi driver keep the network and its password in the
 * driver's own storage, in clear. Removes that copy, once.
 */
void forgetDriverWifi(void);

} // namespace kbview
