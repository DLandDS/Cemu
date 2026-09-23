#include "input/api/RemoteGamePad/RemoteGamePadProtocol.h"

#include <cassert>
#include <limits>

using namespace RemoteGamePadProtocol;

int main()
{
	const auto hello = MakeHello(0x0102030405060708ULL);
	const std::array<uint8_t, 32> example = {
		0x43, 0x47, 0x50, 0x44, 1, 0, 1, 0, 12, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 8, 7, 6, 5, 4, 3, 2, 1, 7, 0, 0, 0
	};
	assert(hello == example);
	Header header;
	assert(DecodeHeader(hello, header) && header.type == 1);

	auto welcome = MakePacket<36>(2, 0x12345678, 0);
	Write64(welcome, 20, 0x0102030405060708ULL);
	Write32(welcome, 28, kFeatures);
	Write16(welcome, 32, 180);
	Welcome reply;
	assert(DecodeHeader(welcome, header) && DecodeWelcome(welcome, header, reply));
	assert(reply.session == 0x12345678 && reply.nonce == 0x0102030405060708ULL);
	welcome[32] = 60;
	assert(!DecodeWelcome(welcome, header, reply));

	auto state = MakePacket<70>(4, reply.session, 1);
	Write32(state, 20, (1u << 0) | (1u << 10) | (1u << 17));
	Write16(state, 24, 32767);
	Write16(state, 26, uint16_t(-32767));
	state[32] = 1;
	Write16(state, 34, 65535);
	Write64(state, 38, 1000000);
	Write32(state, 54, std::bit_cast<uint32_t>(1.0f));
	State decoded;
	assert(DecodeHeader(state, header) && DecodeState(state, header, decoded));
	assert(decoded.buttons == ((1u << 0) | (1u << 10) | (1u << 17)));
	assert(decoded.leftX == 32767 && decoded.leftY == -32767);
	assert(decoded.touchActive && decoded.touchX == 65535);
	assert(decoded.acceleration[2] == 1.0f);
	Header zeroSequence = header;
	zeroSequence.sequence = 0;
	assert(!DecodeState(state, zeroSequence, decoded));
	state[8] = 49;
	assert(!DecodeHeader(state, header));
	state[8] = 50;
	state[32] = 2;
	assert(!DecodeState(state, header, decoded));
	state[32] = 1;
	Write32(state, 58, std::bit_cast<uint32_t>(std::numeric_limits<float>::quiet_NaN()));
	assert(!DecodeState(state, header, decoded));
	assert(NewerSequence(1, 0xffffffffu));
	assert(!NewerSequence(0xffffffffu, 1));
	assert(!NewerSequence(1, 1));

	const auto rumble = MakeRumble(reply.session, 1, true);
	assert(rumble.size() == 24 && rumble[20] == 255 && Read16(rumble, 22) == 250);
	return 0;
}
