#include <fcntl.h>
#include <unistd.h>	// for I/O
#include <cstdlib>	// for std::exit(), EXIT_SUCCESS and EXIT_FAILURE
#include <cstring>	// for std::strcmp() and std::strlen()
#include <cstdint>	// for fixed-width types
#include <cstdio>

//#define BUFFER_SIZE BUFSIZ
#define BUFFER_SIZE 65536

const char helpText[] = "usage: aprepend [--help] <--front || --back> [-b <byte value>] <text>\n" \
			"\n" \
			"function: either appends or prepends text to a data stream\n" \
			"\n" \
			"arguments:\n" \
				"\t[--help]            --> show help text\n" \
				"\t<--front || --back> --> specifies where to put text\n" \
				"\t[-b <byte value>]   --> optional extra byte value\n" \
					"\t\t- gets appended to text when --back is selected\n" \
					"\t\t- gets prepended to text when --front is selected\n" \
				"\t<text>              --> the text to append/prepend\n";

template <typename data_type, size_t destination_length, size_t source_length>
consteval void copyArrayIntoOtherArray(data_type (&destination)[destination_length], const data_type (&source)[source_length], size_t offset) {
	for (size_t i = 0; i < source_length; i++) { destination[offset + i] = source[i]; }
}

template <size_t array_length>
struct char_array_wrapper {
	char array[array_length];
};

template <size_t message_length>
consteval auto processErrorMessage(const char (&message)[message_length]) -> char_array_wrapper<message_length + sizeof("ERROR: ") - 1 + sizeof('\n')> {
	char_array_wrapper<message_length + sizeof("ERROR: ") - 1 + sizeof('\n')> processedMessageWrapper;
	copyArrayIntoOtherArray(processedMessageWrapper.array, "ERROR: ", 0);
	copyArrayIntoOtherArray(processedMessageWrapper.array, message, sizeof("ERROR: ") - 1);
	copyArrayIntoOtherArray(processedMessageWrapper.array, "\n", sizeof("ERROR: ") - 1 + sizeof(message) - 1);
	return processedMessageWrapper;
}

template <size_t message_length>
void writeErrorAndExit(const char (&message)[message_length], int exitCode = EXIT_SUCCESS) noexcept {
	// constexpr temp = processErrorMessage(message);
	// NOTE: The above doesn't work because the compiler can't tell that message is available at compile-time.
	// NOTE: Normally, if this weren't templated, message could also be given at run-time, which means it isn't a constant expression.
	// NOTE: The compiler can't (probably because the standard doesn't support this, this is for the best IMO) tell the difference between the
	// templated version and the non-templated version.
	// SOLUTION: We use the macro below instead, which works great.
	write(STDERR_FILENO, message, message_length);
	std::exit(exitCode);
}

#define reportError(message, exitCode) writeErrorAndExit(processErrorMessage(message).array, exitCode)

void openFloodGates() noexcept {
	char buffer[BUFFER_SIZE];
	while (true) {
		ssize_t bytesRead = read(STDIN_FILENO, buffer, BUFFER_SIZE);
		if (bytesRead == 0) { return; }
		// TODO: Research if you can actually handle less than ideal stuff like you do. I think so, as long as no signal handler is there.
		// NOTE: It can happen if the buffer size is less than the pipe length, because then the call doesn't finish, it just rests on you.
		// NOTE: That means our max is the pipe length, but we have to handle less than that because the user can actually change the pipe length.
		if (bytesRead < BUFFER_SIZE) {
			if (write(STDOUT_FILENO, buffer, bytesRead) == -1) { reportError("failed to write to stdout", EXIT_FAILURE); }
			// TODO: Can the same happen with the write call, or does that fill multiple buffers if it has to?
			// TODO: Do we even want it to?
			return;
		}
		if (bytesRead == -1) { reportError("failed to read from stdin", EXIT_FAILURE); }
		if (write(STDOUT_FILENO, buffer, BUFFER_SIZE) == -1) { reportError("failed to write to stdout", EXIT_FAILURE); }
	}
}

enum class AttachmentLocation {
	none,
	front,
	back
};

namespace flags {
	AttachmentLocation attachmentLocation = AttachmentLocation::none;
	int16_t extraByte = -1;
}

void append(const char* text) noexcept {
	openFloodGates();
	if (write(STDOUT_FILENO, text, std::strlen(text)) == -1) { reportError("failed to write to stdout", EXIT_FAILURE); }
	if (flags::extraByte != -1) {
		unsigned char byte = flags::extraByte;
		if (write(STDOUT_FILENO, &byte, sizeof(byte)) == -1) { reportError("failed to write to stdout", EXIT_FAILURE); }
	}
}

void prepend(const char* text) noexcept {
	if (flags::extraByte != -1) {
		unsigned char byte = flags::extraByte;
		if (write(STDOUT_FILENO, &byte, sizeof(byte)) == -1) { reportError("failed to write to stdout", EXIT_FAILURE); }
	}
	if (write(STDOUT_FILENO, text, std::strlen(text)) == -1) { reportError("failed to write to stdout", EXIT_FAILURE); }
	openFloodGates();
}

unsigned char parseByte(const char* string_input) noexcept {
	const unsigned char* input = (const unsigned char*)string_input;
	unsigned char result = input[0] - (unsigned char)'0';		// NOTE: cast is important for avoided signed overflow, which is undefined behaviour.
	if (result > 9) { reportError("invalid input for optional extra byte", EXIT_SUCCESS); }
	for (size_t i = 1; input[i] != '\0'; i++) {
		if (result >= 100) { reportError("invalid input for optional extra byte", EXIT_SUCCESS); }
		unsigned char digit = input[i] - (unsigned char)'0';
		if (digit > 9) { reportError("invalid input for optional extra byte", EXIT_SUCCESS); }
		result = result * 10 + digit;
	}
	return result;
}

int manageArgs(int argc, const char* const * argv) noexcept {
	int normalArgIndex = 0;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			switch (argv[i][1]) {
			case '-':
				{
					const char* flagContent = argv[i] + 2;
					if (std::strcmp(flagContent, "front") == 0) {
						if (flags::attachmentLocation != AttachmentLocation::none) {
							reportError("you must specify exactly one instance of either --front or --back", EXIT_SUCCESS);
						}
						flags::attachmentLocation = AttachmentLocation::front;
						continue;
					}
					if (std::strcmp(flagContent, "back") == 0) {
						if (flags::attachmentLocation != AttachmentLocation::none) {
							reportError("you must specify exactly one instance of either --front or --back", EXIT_SUCCESS);
						}
						flags::attachmentLocation = AttachmentLocation::back;
						continue;
					}
					if (std::strcmp(flagContent, "help") == 0) {
						if (argc != 2) { reportError("too many args", EXIT_SUCCESS); }
						if (write(STDOUT_FILENO, helpText, sizeof(helpText)) == -1) {
								reportError("TODO: this is annoying, remove this", EXIT_SUCCESS);
						}
						std::exit(EXIT_SUCCESS);
					}
					continue;
				}
			case 'b':
				i++;
				flags::extraByte = parseByte(argv[i]);
				continue;
			}
			continue;
		}
		if (normalArgIndex != 0) { reportError("too many non-flag args", EXIT_SUCCESS); }
		normalArgIndex = i;
	}
	if (flags::attachmentLocation == AttachmentLocation::none) { reportError("you must specify either --front or --back", EXIT_SUCCESS); }
	if (normalArgIndex == 0 && flags::extraByte == -1) { reportError("not enough non-flags args", EXIT_SUCCESS); }
	return normalArgIndex;
}

int main(int argc, const char* const * argv) noexcept {
	int textIndex = manageArgs(argc, argv);
	switch (flags::attachmentLocation) {
	case AttachmentLocation::front: prepend(argv[textIndex]); return EXIT_SUCCESS;
	case AttachmentLocation::back: append(argv[textIndex]); return EXIT_SUCCESS;
	}
}
