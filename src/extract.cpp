/*
This is responsible for extracting the strings, in bulk, from a MessagePack buffer. Creating strings from buffers can
be one of the biggest performance bottlenecks of parsing, but creating an array of extracting strings all at once
provides much better performance. This will parse and produce up to 256 strings at once .The JS parser can call this multiple
times as necessary to get more strings. This must be partially capable of parsing MessagePack so it can know where to
find the string tokens and determine their position and length. All strings are decoded as UTF-8.
*/
#include <v8.h>
#include <node.h>
#include <node_buffer.h>
#include <nan.h>
using namespace v8;

#ifndef thread_local
#ifdef __GNUC__
# define thread_local __thread
#elif __STDC_VERSION__ >= 201112L
# define thread_local _Thread_local
#elif defined(_MSC_VER)
# define thread_local __declspec(thread)
#else
# define thread_local
#endif
#endif

const size_t UTF16_SPACE = 0x4000;

class CustomExternalStringResource : public String::ExternalStringResource {
private:
    const uint16_t *d;

public:
    CustomExternalStringResource() {
    // The Latin data
    this->d = new uint16_t[UTF16_SPACE];
    // Number of Latin characters in the string
}
    ~CustomExternalStringResource() {};

	void Dispose() {
	    delete this;
	}
	const uint16_t *data() const {
	    return this->d;
	}

    size_t length() const {
	    return UTF16_SPACE;
	}

};
class CustomExternalOneByteStringResource : public String::ExternalOneByteStringResource {
private:
    const char *d;
    size_t l;

public:
    CustomExternalOneByteStringResource(char *data, size_t size) {
    // The Latin data
    this->d = data;
    // Number of Latin characters in the string
    this->l = size;
}
    ~CustomExternalOneByteStringResource() {};

	void Dispose() {
	    delete this;
	}
    const char *data() const{
	    return this->d;
	}

    size_t length() const {
	    return this->l;
	}

};

const int MAX_TARGET_SIZE = 255;
typedef int (*token_handler)(uint8_t* source, int position, int size);
token_handler tokenTable[256] = {};
class Extractor {
public:
	uint8_t* source;
	int position = 0;
	int writeSizePosition = 0;
	int stringStart = 0;
	int lastStringEnd = 0;
	uint32_t sizes[0x80];
	CustomExternalStringResource utf16Resource;
	uint16_t* utf16Position;
	CustomExternalOneByteStringResource* utf8Resource;
	Isolate *isolate = Isolate::GetCurrent();
	void readString(int length, bool allowStringBlocks) {
		int start = position;
		int end = position + length;
		while(position < end) {
			if (source[position] < 0x80) // ensure we character is latin and can be decoded as one byte
				position++;
			else {
				position = start;
				break;
			}
		}
		if (position < end) {
			if (lastStringEnd) {
				sizes[writeSizePosition++] = lastStringEnd;
				lastStringEnd = 0;
			}
			// non-latin character(s), need conversion from utf-8 to utf-16
			uint16_t* startPosition = utf16Position;
			while(position < end) {
				uint8_t byte1 = source[position++];
				if ((byte1 & 0x80) == 0) {
					// 1 byte
					*utf16Position = byte1;
					utf16Position++;
				} else if ((byte1 & 0xe0) == 0xc0) {
					// 2 bytes
					uint8_t byte2 = source[position++] & 0x3f;
					*utf16Position = ((byte1 & 0x1f) << 6) | byte2;
					utf16Position++;
				} else if ((byte1 & 0xf0) == 0xe0) {
					// 3 bytes
					uint8_t byte2 = source[position++] & 0x3f;
					uint8_t byte3 = source[position++] & 0x3f;
					*utf16Position = ((byte1 & 0x1f) << 12) | (byte2 << 6) | byte3;
					utf16Position++;
				} else if ((byte1 & 0xf8) == 0xf0) {
					// 4 bytes
					uint8_t byte2 = source[position++] & 0x3f;
					uint8_t byte3 = source[position++] & 0x3f;
					uint8_t byte4 = source[position++] & 0x3f;
					uint32_t unit = ((byte1 & 0x07) << 0x12) | (byte2 << 0x0c) | (byte3 << 0x06) | byte4;
					if (unit > 0xffff) {
						unit -= 0x10000;
						*utf16Position = ((unit >> 10) & 0x3ff) | 0xd800;
						utf16Position++;
						unit = 0xdc00 | (unit & 0x3ff);
					}
					*utf16Position = unit;
					utf16Position++;
				} else {
					*utf16Position = byte1;
				}
			}
			sizes[writeSizePosition++] = utf16Position - startPosition; // record the length in the sizes array
			position = end; // ensure that we are aligned
			return;
		}
		lastStringEnd = end;
	}

	Local<String> setupUtf16() {
		return String::NewExternalTwoByte(isolate, &utf16Resource).ToLocalChecked();
	}

	Local<Value> setupSizesBuffer() {
		return Nan::NewBuffer((char*)sizes, 0x200,
			[](char *, void *) {
				// Don't need to free it
			}, nullptr).ToLocalChecked();
	}

	Local<Value> setupUtf8(uint8_t* inputSource, size_t size) {
		source = inputSource;
		utf8Resource = new CustomExternalOneByteStringResource((char*) inputSource, size);
		return String::NewExternalOneByte(isolate, utf8Resource).ToLocalChecked();
	}

	void extractStrings(int startingPosition, int size) {
		writeSizePosition = 0;
		lastStringEnd = 0;
		utf16Position = (uint16_t*) utf16Resource.data();
		position = startingPosition;
		while (position < size) {
			uint8_t token = source[position++];
			if (token < 0xa0) {
				// all one byte tokens
			} else if (token < 0xc0) {
				// fixstr, we want to convert this
				token -= 0xa0;
				if (token + position > size) {
					Nan::ThrowError("Unexpected end of buffer reading string");
					return;
				}
				readString(token, true);
				if (writeSizePosition >= MAX_TARGET_SIZE)
					break;
			} else if (token <= 0xdb && token >= 0xd9) {
				if (token == 0xd9) { //str 8
					int length = source[position++];
					if (length + position > size) {
						Nan::ThrowError("Unexpected end of buffer reading string");
						return;
					}
					readString(length, true);
				} else if (token == 0xda) { //str 16
					int length = source[position++] << 8;
					length += source[position++];
					if (length + position > size) {
						Nan::ThrowError("Unexpected end of buffer reading string");
						return;
					}
					readString(length, false);
				} else { //str 32
					int length = source[position++] << 24;
					length += source[position++] << 16;
					length += source[position++] << 8;
					length += source[position++];
					if (length + position > size) {
						Nan::ThrowError("Unexpected end of buffer reading string");
						return;
					}
					readString(length, false);
				}
				if (writeSizePosition >= MAX_TARGET_SIZE)
					break;
			} else {
				auto handle = tokenTable[token];
				if ((size_t ) handle < 20) {
					position += (size_t ) handle;
				} else {
					position = tokenTable[token](source, position, size);
					if (position < 0) {
						Nan::ThrowError("Unexpected end of buffer");
						return;
					}
				}
			}
		}

		if (lastStringEnd)
			sizes[writeSizePosition++] = lastStringEnd;
/*#if NODE_VERSION_AT_LEAST(12,0,0)
		return Array::New(isolate, target, writeSizePosition);
#else
		Local<Array> array = Array::New(isolate, writeSizePosition);
		Local<Context> context = Nan::GetCurrentContext();
		for (int i = 0; i < writeSizePosition; i++) {
			array->Set(context, i, target[i]);
		}
		return array;
#endif*/
	}

	/*void error(FastApiCallbackOptions& options) {
		if (options)
			options.fallback = true;
		else
			Nan::ThrowError("Unexpected end of buffer reading string");
	}*/
};
void setupTokenTable() {
	for (int i = 0; i < 256; i++) {
		tokenTable[i] = nullptr;
	}
	// uint8, int8
	tokenTable[0xcc] = tokenTable[0xd0] = (token_handler) 1;
	// uint16, int16, array 16, map 16, fixext 1
	tokenTable[0xcd] = tokenTable[0xd1] = tokenTable[0xdc] = tokenTable[0xde] = tokenTable[0xd4] = (token_handler) 2;
	// fixext 16
	tokenTable[0xd5] = (token_handler) 3;
	// uint32, int32, float32, array 32, map 32
	tokenTable[0xce] = tokenTable[0xd2] = tokenTable[0xca] = tokenTable[0xdd] = tokenTable[0xdf] = (token_handler) 4;
	// fixext 4
	tokenTable[0xd6] = (token_handler) 5;
	// uint64, int64, float64, fixext 8
	tokenTable[0xcf] = tokenTable[0xd3] = tokenTable[0xcb] = (token_handler) 8;
	// fixext 8
	tokenTable[0xd8] = (token_handler) 9;
	// fixext 16
	tokenTable[0xd8] = (token_handler) 17;
	// bin 8
	tokenTable[0xc4] = ([](uint8_t* source, int position, int size) -> int {
		if (position >= size) {
			Nan::ThrowError("Unexpected end of buffer");
			return size;
		}
		int length = source[position++];
		return position + length;
	});
	// bin 16
	tokenTable[0xc5] = ([](uint8_t* source, int position, int size) -> int {
		if (position + 2 > size) {
			Nan::ThrowError("Unexpected end of buffer");
			return size;
		}
		int length = source[position++] << 8;
		length += source[position++];
		return position + length;
	});
	// bin 32
	tokenTable[0xc6] = ([](uint8_t* source, int position, int size) -> int {
		if (position + 4 > size) {
			Nan::ThrowError("Unexpected end of buffer");
			return size;
		}
		int length = source[position++] << 24;
		length += source[position++] << 16;
		length += source[position++] << 8;
		length += source[position++];
		return position + length;
	});
	// ext 8
	tokenTable[0xc7] = ([](uint8_t* source, int position, int size) -> int {
		if (position >= size) {
			Nan::ThrowError("Unexpected end of buffer");
			return size;
		}
		int length = source[position++];
		position++;
		return position + length;
	});
	// ext 16
	tokenTable[0xc8] = ([](uint8_t* source, int position, int size) -> int {
		if (position + 2 > size) {
			Nan::ThrowError("Unexpected end of buffer");
			return size;
		}
		int length = source[position++] << 8;
		length += source[position++];
		position++;
		return position + length;
	});
	// ext 32
	tokenTable[0xc9] = ([](uint8_t* source, int position, int size) -> int {
		if (position + 4 > size) {
			Nan::ThrowError("Unexpected end of buffer");
			return size;
		}
		int length = source[position++] << 24;
		length += source[position++] << 16;
		length += source[position++] << 8;
		length += source[position++];
		position++;
		return position + length;
	});
}

static thread_local Extractor* extractor;

NAN_METHOD(extractStrings) {
	Local<Context> context = Nan::GetCurrentContext();
	int position = Local<Number>::Cast(info[0])->IntegerValue(context).FromJust();
	int size = Local<Number>::Cast(info[1])->IntegerValue(context).FromJust();
	extractor->extractStrings(position, size);
}

NAN_METHOD(newExtractStrings) {
	Local<Context> context = Nan::GetCurrentContext();
	int position = Local<Number>::Cast(info[0])->IntegerValue(context).FromJust();
	int size = Local<Number>::Cast(info[1])->IntegerValue(context).FromJust();
	uint8_t* source = (uint8_t*) node::Buffer::Data(info[2]);
	info.GetReturnValue().Set(extractor->setupUtf8(source, node::Buffer::Length(info[2])));
	extractor->extractStrings(position, size);
}
NAN_METHOD(setupUtf16) {
	info.GetReturnValue().Set(extractor->setupUtf16());
}
NAN_METHOD(setupSizesBuffer) {
	info.GetReturnValue().Set(extractor->setupSizesBuffer());
}


NAN_METHOD(isOneByte) {
	Local<Context> context = Nan::GetCurrentContext();
	info.GetReturnValue().Set(Nan::New<Boolean>(Local<String>::Cast(info[0])->IsOneByte()));
}

void initializeModule(v8::Local<v8::Object> exports) {
	extractor = new Extractor(); // create our thread-local extractor
	setupTokenTable();
	Nan::SetMethod(exports, "newExtractStrings", newExtractStrings);
	Nan::SetMethod(exports, "extractStrings", extractStrings);
	Nan::SetMethod(exports, "setupUtf16", setupUtf16);
	Nan::SetMethod(exports, "setupSizesBuffer", setupSizesBuffer);
	Nan::SetMethod(exports, "isOneByte", isOneByte);
}

NODE_MODULE_CONTEXT_AWARE(extractor, initializeModule);
