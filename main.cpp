/*
NOTE: Let's get a few things straight first before we start:
	- read and write syscalls can read and write less than the requested amount for various reasons
		- signal handlers interrupted them (we don't have that problem in this project)
		- disk is full
		- EOF encountered (this is the reason most of the time (for read that is))
		- when connected to a TTY, read reads up to the line end
			- that is, it can return way less than the wanted amount of bytes because
			the user pressed enter and the data of the line was the only data available
			at the moment
			- I assume this is done like this because of simplicity
				- first, in the use of the read syscall
				- second, in the implementation of reading from TTY
		- etc... (I haven't read of any others, but apparently there are more)
	- just to be safe (which is a super idea), we're not going to assume less than optimal byte
	transfer means an EOF was encountered

	- pipes are implemented under the hood as circular buffers of pointers to physical memory pages
	- we can copy the data of the pages of pipe A directly to the pages of pipe B using syscalls,
	instead of reading and writing (which would first transfer the data to and from user mem and
	also require walking the page table, both of which aren't the cheapest operations)
	- this is what "splice" does for us (it also tries to move the data by just copying the pointers
	to the physical pages, but that only works if both ends of the pipe cooperate, which isn't the case
	most of the time)
	- "splice" only works if either stdin or stdout is a pipe, or if both are pipes
	- if that isn't the case, we have to default to reading and writing
	- BTW: "splice" can transfer less than the given amount of bytes as well, we have to deal with that
*/

#include <cstdlib>	// for std::exit(), EXIT_SUCCESS and EXIT_FAILURE, as well as every other syscall we use
#ifndef PLATFORM_WINDOWS
#include <unistd.h>	// for I/O
#include <sys/stat.h>	// for fstat supporting structures
#include <fcntl.h>	// for fcntl supporting structures
#else
#include <io.h>		// for Windows I/O
#endif
#include <cstdio>	// for BUFSIZ
#include <cstdint>	// for fixed-width integers
#include <cstring>	// for std::strcmp() and std::strlen()

#ifdef PLATFORM_WINDOWS
using ssize_t = int;

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define read(fd, buffer, count) _read(fd, buffer, count)
#define write(fd, buffer, count) _write(fd, buffer, count)
#endif

const char helpText[] = "usage: aprepend <--front || --back> [-b <byte value>] <text>\n" \
			"       aprepend --help\n" \
			"\n" \
			"function: either appends or prepends text to a data stream\n" \
			"\n" \
			"arguments:\n" \
				"\t[--help]            --> show help text\n" \
				"\t<--front || --back> --> specifies where to put text\n" \
				"\t[-b <byte value>]   --> optional extra byte value\n" \
					"\t\t- gets appended to text when --back is selected\n" \
					"\t\t- gets prepended to text when --front is selected\n" \
					"\t\t- when -b is used, text can be omitted\n" \
				"\t<text>              --> the text to append/prepend\n";

// ERROR OUTPUT SYSTEM BEGIN -------------------------------------------------

template <typename data_type, size_t destination_length, size_t source_length>
// NOTE: MSVC doesn't want to evaluate these compile-time functions at compile-time, so we're allowing it not
// to by declaring them constexpr when compiling for Windows.
// NOTE: BTW: constexpr functions have to be compile-time evaluatable (see constexpr definition), they just don't always have
// to get compile-time evaluated (the decision is based on the compile-time evaluatability of the parameters).
// I don't know if MSVC adheres to this, but if it does, we can conclude that MSVC has a problem with the array reference
// inputs to the functions, not the functions themselves.
#ifndef PLATFORM_WINDOWS
consteval
#else
constexpr
#endif
void copyArrayIntoOtherArray(data_type (&destination)[destination_length], const data_type (&source)[source_length], size_t offset) {
	for (size_t i = 0; i < source_length; i++) { destination[offset + i] = source[i]; }
}

template <size_t array_length>
struct char_array_wrapper { char array[array_length]; };

template <size_t message_length>
#ifndef PLATFORM_WINDOWS
consteval
#else
constexpr
#endif
auto processErrorMessage(const char (&message)[message_length]) -> char_array_wrapper<message_length + sizeof("ERROR: ") - 1 + sizeof('\n')> {
	char_array_wrapper<message_length + sizeof("ERROR: ") - 1 + sizeof('\n')> processedMessageWrapper;
	copyArrayIntoOtherArray(processedMessageWrapper.array, "ERROR: ", 0);
	copyArrayIntoOtherArray(processedMessageWrapper.array, message, sizeof("ERROR: ") - 1);
	copyArrayIntoOtherArray(processedMessageWrapper.array, "\n", sizeof("ERROR: ") - 1 + sizeof(message) - 1);
	return processedMessageWrapper;
}

template <size_t message_length>
void writeErrorAndExit(const char (&message)[message_length], int exitCode) noexcept {
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

// ERROR OUTPUT SYSTEM END ----------------------------------------------------

// FAST TRANSFER MECHANISM START ----------------------------------------------

void openFloodGates() noexcept {
#ifndef PLATFORM_WINDOWS
	struct stat status;

	int stdinPipeBufferSize;
	if (fstat(STDIN_FILENO, &status) == 0 && S_ISFIFO(status.st_mode)) { stdinPipeBufferSize = fcntl(STDIN_FILENO, F_GETPIPE_SZ); }
	else { stdinPipeBufferSize = -1; } // NOTE: If we fail to get the status (which shouldn't ever really happen), not all hope is lost. The other fd might still work.

	int stdoutPipeBufferSize;
	if (fstat(STDOUT_FILENO, &status) == 0 && S_ISFIFO(status.st_mode)) { stdoutPipeBufferSize = fcntl(STDOUT_FILENO, F_GETPIPE_SZ); }
	else { stdoutPipeBufferSize = -1; }

	int spliceStepSizeInBytes;
	// NOTE: If only one buffer size couldn't be gotten (shouldn't ever really happen, unless only one fd is a pipe), use the other one.
	if (stdinPipeBufferSize != -1) {
		if (stdoutPipeBufferSize != -1) {
			// NOTE: We use the bigger of the two pipe buffers as the splice size to save on syscalls.
			// NOTE: Don't worry about finding the smallest common multiple of the pipe buffers to keep synchronization
			// of the buffers. The buffers are ring buffers, so it doesn't matter.
			spliceStepSizeInBytes = stdinPipeBufferSize > stdoutPipeBufferSize ? stdinPipeBufferSize : stdoutPipeBufferSize;
		}
		else { spliceStepSizeInBytes = stdinPipeBufferSize; }
	} else {
		if (stdoutPipeBufferSize != -1) { spliceStepSizeInBytes = stdoutPipeBufferSize; }
		else { goto read_write_transfer; }
	}

	while (true) {
		// NOTE: There's no need to splice the remaining bytes if bytesSpliced is less than expected.
		// NOTE: We just keep splicing at full speed and stop once 0 comes out. That'll still get all
		// the bytes and everything will be simpler and faster.
		ssize_t bytesSpliced = splice(STDIN_FILENO, nullptr, 
					      STDOUT_FILENO, nullptr, 
					      spliceStepSizeInBytes, 
					      SPLICE_F_MOVE | SPLICE_F_MORE);
		// NOTE: SPLICE_F_MOVE is doesn't mean anything in the current kernel. Still, they might implement the correct functionality
		// in a future version, in which case this flag will be useful. It specifies that the data should be moved from the pipe
		// if possible.
		// NOTE: SPLICE_F_MORE just means that more data is coming in subsequent calls. Useful hint for kernel when stdout is a socket.
		if (bytesSpliced == 0) { return; }
		// NOTE: The most common case for an error here is if stdin and stdout refer to the same pipe.
		// I can't find a way to preemptively check this condition though, because the pipes can be anonymous and not on the fs.
		// It's for the best though. Not preemptively checking it is better (simplicity + performance).
		// NOTE: There are 1 or 2 other possibilities, the goto is the correct reaction to all of them though.
		if (bytesSpliced == -1) { goto read_write_transfer; }
	}

read_write_transfer:
#endif

	char buffer[BUFSIZ];
	while (true) {
		ssize_t bytesRead = read(STDIN_FILENO, buffer, BUFSIZ);
		if (bytesRead == 0) { return; }
		if (bytesRead == -1) { reportError("failed to read from stdin", EXIT_FAILURE); }

		const char* bufferPointer = buffer;
		ssize_t bytesWritten;
		while ((bytesWritten = write(STDOUT_FILENO, bufferPointer, bytesRead)) != bytesRead) {
			if (bytesWritten == -1) { reportError("failed to write to stdout", EXIT_FAILURE); }
			bufferPointer += bytesWritten;
			bytesRead -= bytesWritten;
		}
	}
}

// FAST TRANSFER MECHANISM END -------------------------------------------------

// COMMAND-LINE PARSER START ---------------------------------------------------

enum class AttachmentLocation {
	none,
	front,
	back
};

namespace flags {
	AttachmentLocation textAttachmentLocation = AttachmentLocation::none;
	int16_t extraByte = -1;
}

unsigned char parseByte(const char* string_input) noexcept {
	// NOTE: Trust me, this may look weird, but it's the best way of doing this.
	// NOTE: I've unrolled the first three iterations of the loop because we don't need the (result >= 100) check in those.
	// NOTE: Also, the fact that we don't explicitly check for NUL everywhere is on purpose. It's implied with (digit > 9).
	// NOTE: We check for NUL in the loop on purpose though, since after three characters, NUL's are way more likely to come.
	// The explicit check is there to exit early in such cases.

	const unsigned char* input = (const unsigned char*)string_input;

	unsigned char result = input[0] - (unsigned char)'0';		// NOTE: cast is important for avoided signed overflow, which is undefined behaviour.
	if (result > 9) { reportError("invalid input for optional extra byte", EXIT_SUCCESS); }

	unsigned char digit = input[1] - (unsigned char)'0';
	if (digit > 9) { reportError("invalid input for optional extra byte", EXIT_SUCCESS); }
	result = result * 10 + digit;

	digit = input[2] - (unsigned char)'0';
	if (digit > 9) { reportError("invalid input for optional extra byte", EXIT_SUCCESS); }
	result = result * 10 + digit;

	for (size_t i = 3; input[i] != '\0'; i++) {
		if (result >= 100) { reportError("invalid input for optional extra byte", EXIT_SUCCESS); }
		digit = input[i] - (unsigned char)'0';
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
						if (flags::textAttachmentLocation != AttachmentLocation::none) {
							reportError("you must specify exactly one instance of either --front or --back", EXIT_SUCCESS);
						}
						flags::textAttachmentLocation = AttachmentLocation::front;
						continue;
					}
					if (std::strcmp(flagContent, "back") == 0) {
						if (flags::textAttachmentLocation != AttachmentLocation::none) {
							reportError("you must specify exactly one instance of either --front or --back", EXIT_SUCCESS);
						}
						flags::textAttachmentLocation = AttachmentLocation::back;
						continue;
					}
					if (std::strcmp(flagContent, "help") == 0) {
						if (argc != 2) { reportError("too many args", EXIT_SUCCESS); }
						if (write(STDOUT_FILENO, helpText, sizeof(helpText) - 1) == -1) {
							reportError("failed to write to stdout", EXIT_FAILURE);
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
	if (flags::textAttachmentLocation == AttachmentLocation::none) { reportError("you must specify either --front or --back", EXIT_SUCCESS); }
	if (normalArgIndex == 0 && flags::extraByte == -1) { reportError("not enough non-flags args", EXIT_SUCCESS); }
	return normalArgIndex;
}

// COMMAND-LINE PARSER END -----------------------------------------------------

// MAIN LOGIC START ------------------------------------------------------------

void append(const char* text) noexcept {
	openFloodGates();
	if (text != nullptr && write(STDOUT_FILENO, text, std::strlen(text)) == -1) { reportError("failed to write to stdout", EXIT_FAILURE); }
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
	if (text != nullptr && write(STDOUT_FILENO, text, std::strlen(text)) == -1) { reportError("failed to write to stdout", EXIT_FAILURE); }
	openFloodGates();
}

int main(int argc, const char* const * argv) noexcept {
	int textIndex = manageArgs(argc, argv);
	switch (flags::textAttachmentLocation) {
	case AttachmentLocation::front: prepend(textIndex == 0 ? nullptr : argv[textIndex]); return EXIT_SUCCESS;
	case AttachmentLocation::back: append(textIndex == 0 ? nullptr : argv[textIndex]); return EXIT_SUCCESS;
	}
}

// MAIN LOGIC END --------------------------------------------------------------
