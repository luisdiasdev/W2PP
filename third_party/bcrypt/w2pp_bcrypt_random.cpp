/*
 * Salt generation for the vendored BCrypt implementation.
 *
 * The OpenBSD original calls arc4random_buf(), which is not available on
 * MSVC. std::random_device is backed by the operating system CSPRNG on both
 * supported toolchains (RtlGenRandom on Windows, /dev/urandom or getrandom on
 * Linux), so it gives each hash an independent 16-byte salt.
 */

#include <cstddef>
#include <cstdint>
#include <random>

extern "C" void w2pp_bcrypt_random_bytes(std::uint8_t *buf, std::size_t len);

extern "C" void w2pp_bcrypt_random_bytes(std::uint8_t *buf, std::size_t len)
{
	std::random_device source;

	for (std::size_t i = 0; i < len; i += sizeof(unsigned int)) {
		unsigned int value = source();
		for (std::size_t b = 0; b < sizeof(unsigned int) && i + b < len; ++b)
			buf[i + b] = static_cast<std::uint8_t>(value >> (b * 8));
	}
}
