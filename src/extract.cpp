/*
This is responsible for extracting the strings, in bulk, from a MessagePack buffer. Creating strings from buffers can
be one of the biggest performance bottlenecks of parsing, but creating an array of extracting strings all at once
provides much better performance. This will parse and produce up to 256 strings at once .The JS parser can call this multiple
times as necessary to get more strings. This must be partially capable of parsing MessagePack so it can know where to
find the string tokens and determine their position and length. All strings are decoded as UTF-8.
*/
#include <v8.h>
#include <napi.h>
#include <node_api.h>

using namespace Napi;

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

const int MAX_TARGET_SIZE = 255;
typedef int (*token_handler)(Env env, uint8_t* source, int position, int size);
token_handler tokenTable[256] = {};
class Extractor {
public:
	v8::Local<v8::Value> target[MAX_TARGET_SIZE + 1]; // leave one for the queued string

	uint8_t* source;
	int position = 0;
	int writePosition = 0;
	int stringStart = 0;
	int lastStringEnd = 0;
	v8::Isolate *isolate = v8::Isolate::GetCurrent();

	void readString(Env env, int length, bool allowStringBlocks) {
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
				target[writePosition++] = v8::String::NewFromOneByte(isolate,  (uint8_t*) source + stringStart, v8::NewStringType::kNormal, lastStringEnd - stringStart).ToLocalChecked();
				lastStringEnd = 0;
			}
			// use standard utf-8 conversion
			target[writePosition++] = v8::String::NewFromUtf8(isolate, (char*) source + start, v8::NewStringType::kNormal, length).ToLocalChecked();
			position = end;
			return;
		}

		if (lastStringEnd) {
			if (start - lastStringEnd > 40 || end - stringStart > 6000) {
				target[writePosition++] = v8::String::NewFromOneByte(isolate, (uint8_t*) source + stringStart, v8::NewStringType::kNormal, lastStringEnd - stringStart).ToLocalChecked();
				stringStart = start;
			}
		} else {
			stringStart = start;
		}
		lastStringEnd = end;
	}

	napi_value extractStrings(Env env, int startingPosition, int size, uint8_t* inputSource) {
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
				if (token + position > size) {
					TypeError::New(env, "Unexpected end of buffer reading string").ThrowAsJavaScriptException();
					return env.Null();
				}
				readString(env,token, true);
				if (writePosition >= MAX_TARGET_SIZE)
					break;
			} else if (token <= 0xdb && token >= 0xd9) {
				if (token == 0xd9) { //str 8
					if (position >= size) {
						TypeError::New(env, "Unexpected end of buffer reading string").ThrowAsJavaScriptException();
						return env.Null();
					}
					int length = source[position++];
					if (length + position > size) {
						TypeError::New(env, "Unexpected end of buffer reading string").ThrowAsJavaScriptException();
						return env.Null();
					}
					readString(env,length, true);
				} else if (token == 0xda) { //str 16
					if (2 + position > size) {
						TypeError::New(env, "Unexpected end of buffer reading string").ThrowAsJavaScriptException();
						return env.Null();
					}
					int length = source[position++] << 8;
					length += source[position++];
					if (length + position > size) {
						TypeError::New(env, "Unexpected end of buffer reading string").ThrowAsJavaScriptException();
						return env.Null();
					}
					readString(env,length, false);
				} else { //str 32
					if (4 + position > size) {
						TypeError::New(env, "Unexpected end of buffer reading string").ThrowAsJavaScriptException();
						return env.Null();
					}
					int length = source[position++] << 24;
					length += source[position++] << 16;
					length += source[position++] << 8;
					length += source[position++];
					if (length + position > size) {
						TypeError::New(env, "Unexpected end of buffer reading string").ThrowAsJavaScriptException();
						return env.Null();
					}
					readString(env, length, false);
				}
				if (writePosition >= MAX_TARGET_SIZE)
					break;
			} else {
				auto handle = tokenTable[token];
				if ((size_t ) handle < 20) {
					position += (size_t ) handle;
				} else {
					position = tokenTable[token](env, source, position, size);
					if (position < 0) {
						TypeError::New(env, "Unexpected end of buffer").ThrowAsJavaScriptException();
						return env.Null();
					}
				}
			}
		}
		v8::Local<v8::Value> v8ReturnValue;
		if (lastStringEnd) {
			if (writePosition == 0) {
				v8ReturnValue = v8::String::NewFromOneByte(isolate, (uint8_t*) source + stringStart, v8::NewStringType::kNormal, lastStringEnd - stringStart).ToLocalChecked();
				goto done;
			}
			target[writePosition++] = v8::String::NewFromOneByte(isolate, (uint8_t*) source + stringStart, v8::NewStringType::kNormal, lastStringEnd - stringStart).ToLocalChecked();
		} else if (writePosition == 1) {
			v8ReturnValue = target[0];
			goto done;
		}
//#if NODE_VERSION_AT_LEAST(12,0,0)
		v8ReturnValue = v8::Array::New(isolate, target, writePosition);
		done:
		napi_value returnValue;
		memcpy(&returnValue, &v8ReturnValue, sizeof(napi_value));
		return returnValue;
/*#else
		v8::Local<v8::Array> array = v8::Array::New(isolate, writePosition);
		v8::Local<v8::Context> context = v8::Isolate::GetCurrentContext();
		for (int i = 0; i < writePosition; i++) {
			array->Set(context, i, target[i]);
		}
		return array;*/
//#endif
	}
};

void setupTokenTable(Env env) {
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
	tokenTable[0xd7] = (token_handler) 9;
	// fixext 16
	tokenTable[0xd8] = (token_handler) 17;
	// bin 8
	tokenTable[0xc4] = ([](Env env, uint8_t* source, int position, int size) -> int {
		if (position >= size) {
			TypeError::New(env, "Unexpected end of buffer").ThrowAsJavaScriptException();
			return size;
		}
		int length = source[position++];
		return position + length;
	});
	// bin 16
	tokenTable[0xc5] = ([](Env env, uint8_t* source, int position, int size) -> int {
		if (position + 2 > size) {
			TypeError::New(env, "Unexpected end of buffer").ThrowAsJavaScriptException();
			return size;
		}
		int length = source[position++] << 8;
		length += source[position++];
		return position + length;
	});
	// bin 32
	tokenTable[0xc6] = ([](Env env, uint8_t* source, int position, int size) -> int {
		if (position + 4 > size)
			return -1;
		int length = source[position++] << 24;
		length += source[position++] << 16;
		length += source[position++] << 8;
		length += source[position++];
		return position + length;
	});
	// ext 8
	tokenTable[0xc7] = ([](Env env, uint8_t* source, int position, int size) -> int {
		if (position >= size)
			return -1;
		int length = source[position++];
		position++;
		return position + length;
	});
	// ext 16
	tokenTable[0xc8] = ([](Env env, uint8_t* source, int position, int size) -> int {
		if (position + 2 > size)
			return -1;
		int length = source[position++] << 8;
		length += source[position++];
		position++;
		return position + length;
	});
	// ext 32
	tokenTable[0xc9] = ([](Env env, uint8_t* source, int position, int size) -> int {
		if (position + 4 > size)
			return -1;
		int length = source[position++] << 24;
		length += source[position++] << 16;
		length += source[position++] << 8;
		length += source[position++];
		position++;
		return position + length;
	});
}

static thread_local Extractor* extractor;

napi_value extractStrings(const CallbackInfo& info) {
  Env env = info.Env();
	int position = info[0].As<Number>();
	int size = info[1].As<Number>();
	if (info[2].IsTypedArray()) {
		TypedArray typedArray = info[2].As<TypedArray>();
    	uint8_t* source = ((uint8_t*) typedArray.ArrayBuffer().Data()) + typedArray.ByteOffset();
		return extractor->extractStrings(env, position, size, source);
	}
  return env.Undefined();
}

napi_value isOneByte(const CallbackInfo& info) {
  	Env env = info.Env();
	size_t length;
	napi_get_value_string_latin1(env, info[0], nullptr, 0, &length);

  	return Boolean::New(env, length == 1);
}

Object Init(Env env, Object exports) {
	extractor = new Extractor(); // create our thread-local extractor
	setupTokenTable(env);
	exports.Set(
    String::New(env, "extractStrings"),
    Function::New(env, extractStrings)
  );
	exports.Set(
    String::New(env, "isOneByte"),
    Function::New(env, isOneByte)
  );
	return exports;
}

NODE_API_MODULE(extractor, Init)