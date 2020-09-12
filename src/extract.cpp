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

const int MAX_TARGET_SIZE = 255;
typedef int (*token_handler)(int token, uint8_t* source, int position);
token_handler tokenTable[256] = {};
class Extractor {
public:
	v8::Local<v8::Value> target[MAX_TARGET_SIZE + 1]; // leave one for the queued string

	uint8_t* source;
	int position = 0;
	int writePosition = 0;
	int stringStart = 0;
	int lastStringEnd = 0;
	Isolate *isolate = Isolate::GetCurrent();
	void readString(int length, bool allowStringBlocks) {
		int start = position;
		int end = position + length;
		if (allowStringBlocks) { // for larger strings, we don't bother to check every character for being latin, and just go right to creating a new string
			while(position < end) {
				if (source[position] < 0x80) // ensure we character is latin and can be decoded as one byte
					position++;
				else {
					break;
				}
			}
		}
		if (position < end) {
			// non-latin character
			if (lastStringEnd) {
				target[writePosition++] = String::NewFromOneByte(isolate,  (uint8_t*) source + stringStart, v8::NewStringType::kNormal, lastStringEnd - stringStart).ToLocalChecked();
				lastStringEnd = 0;
			}
			// use standard utf-8 conversion
			target[writePosition++] = Nan::New<v8::String>((char*) source + start, (int) length).ToLocalChecked();
			position = end;
			return;
		}

		if (lastStringEnd) {
			if (start - lastStringEnd > 40 || end - stringStart > 4000) {
				target[writePosition++] = String::NewFromOneByte(isolate, (uint8_t*) source + stringStart, v8::NewStringType::kNormal, lastStringEnd - stringStart).ToLocalChecked();
				stringStart = start;
			}
		} else {
			stringStart = start;
		}
		lastStringEnd = end;
	}

	Local<Array> extractStrings(int startingPosition, int size, uint8_t* inputSource) {
		writePosition = 0;
		lastStringEnd = 0;
		position = startingPosition;
		source = inputSource;
		while (position < size) {
			uint8_t token = source[position++];
			if (token < 0xa0) {
				// all one byte tokens
			} else if (token < 0xc0) {
				// fixstr, we want to convert this
				token -= 0xa0;
				readString(token, true);
				if (writePosition >= MAX_TARGET_SIZE)
					break;
			} else if (token <= 0xda && token >= 0xd9) {
				if (token == 0xd9) { //str 8
					int length = source[position++];
					readString(length, true);
				} else if (token == 0xda) { //str 16
					int length = source[position++] << 8;
					length += source[position++];
					readString(length, false);
				} else { //str 32
					int length = source[position++] << 24;
					length += source[position++] << 16;
					length += source[position++] << 8;
					length += source[position++];
					readString(length, false);
				}
				if (writePosition >= MAX_TARGET_SIZE)
					break;
			} else {
				if (tokenTable[token]) {
					position = tokenTable[token](token, source, position);
				}
			}
		}
	//	Isolate *isolate = Isolate::GetCurrent();
		if (lastStringEnd)
			target[writePosition++] = String::NewFromOneByte(isolate, (uint8_t*) source + stringStart, v8::NewStringType::kNormal, lastStringEnd - stringStart).ToLocalChecked();
#if NODE_VERSION_AT_LEAST(12,0,0)
		return Array::New(isolate, target, writePosition);
#else
		Local<Array> array = Array::New(isolate, writePosition);
		Local<Context> context = Nan::GetCurrentContext();
		for (int i = 0; i < writePosition; i++) {
			array->Set(context, i, target[i]);
		}
		return array;
#endif
	}
};
void setupTokenTable() {
	for (int i = 0; i < 256; i++) {
		tokenTable[i] = nullptr;	
	}
	// uint8, int8
	tokenTable[0xcc] = tokenTable[0xd0] = ([](int token, uint8_t* source, int position) -> int {
		return position + 1;
	});
	// uint16, int16, array 16, map 16, fixext 1
	tokenTable[0xcd] = tokenTable[0xd1] = tokenTable[0xdc] = tokenTable[0xde] = tokenTable[0xd4] = ([](int token, uint8_t* source, int position) -> int {
		return position + 2;;
	});
	// fixext 16
	tokenTable[0xd5] = ([](int token, uint8_t* source, int position) -> int {
		return position + 3;
	});
	// uint32, int32, float32, array 32, map 32
	tokenTable[0xce] = tokenTable[0xd2] = tokenTable[0xca] = tokenTable[0xdd] = tokenTable[0xdf] = ([](int token, uint8_t* source, int position) -> int {
		return position + 4;
	});
	// fixext 4
	tokenTable[0xd6] = ([](int token, uint8_t* source, int position) -> int {
		return position + 5;
	});
	// uint64, int64, float64, fixext 8
	tokenTable[0xcf] = tokenTable[0xd3] = tokenTable[0xcb] = ([](int token, uint8_t* source, int position) -> int {
		return position + 8;
	});
	// fixext 8
	tokenTable[0xd8] = ([](int token, uint8_t* source, int position) -> int {
		return position + 9;
	});
	// fixext 16
	tokenTable[0xd8] = ([](int token, uint8_t* source, int position) -> int {
		return position + 17;
	});
	// bin 8
	tokenTable[0xc4] = ([](int token, uint8_t* source, int position) -> int {
		int length = source[position++];
		return position + length;
	});
	// bin 16
	tokenTable[0xc5] = ([](int token, uint8_t* source, int position) -> int {
		int length = source[position++] << 8;
		length += source[position++];
		return position + length;
	});
	// bin 32
	tokenTable[0xc6] = ([](int token, uint8_t* source, int position) -> int {
		int length = source[position++] << 24;
		length += source[position++] << 16;
		length += source[position++] << 8;
		length += source[position++];
		return position + length;
	});
	// ext 8
	tokenTable[0xc7] = ([](int token, uint8_t* source, int position) -> int {
		int length = source[position++];
		position++;
		return position + length;
	});
	// ext 16
	tokenTable[0xc8] = ([](int token, uint8_t* source, int position) -> int {
		int length = source[position++] << 8;
		length += source[position++];
		position++;
		return position + length;
	});
	// ext 32
	tokenTable[0xc9] = ([](int token, uint8_t* source, int position) -> int {
		int length = source[position++] << 24;
		length += source[position++] << 16;
		length += source[position++] << 8;
		length += source[position++];
		position++;
		return position + length;
	});
}

#ifdef thread_local
static thread_local Extractor* extractor;
#else
static Extractor* extractor;
#endif
NAN_METHOD(extractStrings) {
	Local<Context> context = Nan::GetCurrentContext();
	int position = Local<Number>::Cast(info[0])->IntegerValue(context).FromJust();
	int size = Local<Number>::Cast(info[1])->IntegerValue(context).FromJust();
	if (info[2]->IsArrayBufferView()) {
		uint8_t* source = (uint8_t*) node::Buffer::Data(info[2]);
		info.GetReturnValue().Set(extractor->extractStrings(position, size, source));
	}
}

NAN_METHOD(isOneByte) {
	Local<Context> context = Nan::GetCurrentContext();
	info.GetReturnValue().Set(Nan::New<Boolean>(Local<String>::Cast(info[0])->IsOneByte()));
}

void initializeModule(v8::Local<v8::Object> exports) {
	extractor = new Extractor(); // create our thread-local extractor
	setupTokenTable();
	Nan::SetMethod(exports, "extractStrings", extractStrings);
	Nan::SetMethod(exports, "isOneByte", isOneByte);
}

NODE_MODULE_CONTEXT_AWARE(extractor, initializeModule);
