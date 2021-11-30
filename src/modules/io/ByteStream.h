/**
 * @file
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "core/String.h"
#include <SDL_endian.h>
#include <limits.h>
#include "core/Common.h"
#include "core/StandardLib.h"
#include "core/Assert.h"
#include "core/collection/Buffer.h"

namespace io {

#define BYTE_MASK 0XFF
#define WORD_MASK 0XFFFF

// TODO: convert to io::WriteStream
class ByteStream {
private:
	typedef core::Buffer<uint8_t> Buffer;
	Buffer _buffer;
	int _pos = 0;

	inline int size() const {
		return (int)(_buffer.size() - _pos);
	}

	inline Buffer::const_iterator begin() const {
		return _buffer.begin() + _pos;
	}

	inline Buffer::iterator begin() {
		return _buffer.begin() + _pos;
	}

public:
	ByteStream(int size = 0);

	void addByte(uint8_t byte);
	void addInt(int32_t dword);

	uint8_t readByte();
	int32_t readInt();

	// get the raw data pointer for the buffer
	const uint8_t* getBuffer() const;

	void append(const uint8_t *buf, size_t size);

	bool empty() const;

	// clear the buffer if it's no longer needed
	void clear();

	// return the amount of bytes in the buffer
	size_t getSize() const;

	void resize(size_t size);
};

inline bool ByteStream::empty() const {
	return size() <= 0;
}

inline void ByteStream::resize(size_t size) {
	_buffer.resize(size);
}

inline void ByteStream::append(const uint8_t *buf, size_t size) {
	_buffer.reserve(_buffer.size() + size);
	_buffer.insert(_buffer.end(), buf, buf + size);
}

inline const uint8_t* ByteStream::getBuffer() const {
	return &_buffer[0] + _pos;
}

inline void ByteStream::clear() {
	_buffer.clear();
}

inline size_t ByteStream::getSize() const {
	return _buffer.size() - _pos;
}

inline void ByteStream::addByte(uint8_t byte) {
	_buffer.push_back(byte);
}

inline void ByteStream::addInt(int32_t dword) {
	int32_t swappedDWord = SDL_SwapLE32(dword);
	_buffer.push_back(uint8_t(swappedDWord));
	_buffer.push_back(uint8_t(swappedDWord >>= CHAR_BIT));
	_buffer.push_back(uint8_t(swappedDWord >>= CHAR_BIT));
	_buffer.push_back(uint8_t(swappedDWord >> CHAR_BIT));
}

inline uint8_t ByteStream::readByte() {
	core_assert(size() > 0);
	const uint8_t byte = _buffer[_pos];
	++_pos;
	return byte;
}

inline int32_t ByteStream::readInt() {
	core_assert(size() >= 4);
	int32_t word;
	core_memcpy(&word, getBuffer(), 4);
	const int32_t val = SDL_SwapLE32(word);
	_pos += 4;
	return val;
}

}
