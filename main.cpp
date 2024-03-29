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
	most of the time TODO: This sounds sus, you should fact check this. --> It seems I meant the right thing but I said it suboptimally. I meant that vmsplice needs to use GIFT for splice MOVE to really move things instead of just copying. But if the source program uses write to write into the pipe, splice should still be able to move. You should add that to the comment.)
	- "splice" only works if either stdin or stdout is a pipe, or if both are pipes
	- if that isn't the case, we have to default to reading and writing
	- BTW: "splice" can transfer less than the given amount of bytes as well, we have to deal with that
*/

#include <cstdlib>	// most syscalls are in here, also it has some common lib functions like std::exit() that we use

#ifndef PLATFORM_WINDOWS

#include <unistd.h>	// for raw I/O
#include <sys/stat.h>	// for fstat() support
#include <fcntl.h>	// for fcntl() support
#include <sys/mman.h>	// for mmap() support

using sioret_t = ssize_t;

#else

#include <io.h>		// for Windows raw I/O

using sioret_t = int;

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define read(fd, buffer, count) _read(fd, buffer, count)
#define write(fd, buffer, count) _write(fd, buffer, count)

#endif

#include <cstdio>	// for BUFSIZ

#include <cstdint>	// for fixed-width integers

#include <cstring>	// for std::strcmp() and std::strlen()

// TODO: For modes with read/write, you could make 2x speed improvement by reading and writing in at the same time.
// Just add a couple threads and double buffer the data, read into one while writing from the other and boom, 2x speedup.

// TODO: Consider making use of fadvise and madvise in order to make things faster (remember to use the posix versions, for either one or both of them, there was a special function name for the posix version, I don't remember anymore).

const char helpText[] = "usage: aprepend <--front || --back> [-b <byte value>] <text>\n" \
			"       aprepend <--help>\n" \
			"\n" \
			"function: either appends or prepends text to a data stream\n" \
			"\n" \
			"arguments:\n" \
				"\t<--help>            --> show help text\n" \
				"\t<--front || --back> --> specifies where to put text\n" \
				"\t[-b <byte value>]   --> optional extra byte value\n" \
					"\t\t- gets appended to text when --back is selected\n" \
					"\t\t- gets prepended to text when --front is selected\n" \
					"\t\t- when -b is used, text can be omitted\n" \
				"\t<text>              --> the text to append/prepend\n";

// ERROR OUTPUT SYSTEM BEGIN -------------------------------------------------

template <size_t message_length>
void writeErrorAndExit(const char (&message)[message_length], int exitCode) noexcept {
	// constexpr auto temp = processErrorMessage(message);
	// NOTE: The above doesn't work because it technically isn't guaranteed that the message is a constant-expression, it could be runtime dependant and such.
	// Instead, we simply use the macro below.
	// NOTE: Even if this could be a consteval function, it still wouldn't work, because parameters of consteval functions are not constant expressions (from the perspective of the consteval function body). If they were,
	// it would cause problems with the type system, like the fact that the return type of a function could be dependent on the argument values of the function. C++ has no
	// way of representing that, and it would be overly complicated to use even if it were supported, so it's good that it isn't.
	// NOTE: Even though the above is true, you can still pass consteval function parameters to consteval sub-functions, although you can't use the return values of those sub-functions as constant expressions.
	// It's a very elegant system. I've got more in-depth comments in other projects, in case you don't remember all of the details. Alternatively, you can go to the cppreference website.
	// (Note that I'm talking to myself, so feel free to ignore this if you like.)
	write(STDERR_FILENO, message, message_length - 1);
	std::exit(exitCode);
}

#define REPORT_ERROR_AND_EXIT(message, exitCode) writeErrorAndExit("ERROR: " message "\n", exitCode)

// ERROR OUTPUT SYSTEM END ----------------------------------------------------

// FAST TRANSFER MECHANISM START ----------------------------------------------

void openFloodGates() noexcept {
#ifndef PLATFORM_WINDOWS

	struct stat status;

	int stdoutPipeBufferSize;
	if (fstat(STDOUT_FILENO, &status) == 0 && S_ISFIFO(status.st_mode)) { stdoutPipeBufferSize = fcntl(STDOUT_FILENO, F_GETPIPE_SZ); }
	else { stdoutPipeBufferSize = -1; }

	// NOTE: If we fail to get the status (which shouldn't ever really happen), not all hope is lost. The other fd might still work.

	int stdinPipeBufferSize;
	if (fstat(STDIN_FILENO, &status) == 0) {
		if (S_ISFIFO(status.st_mode)) { stdinPipeBufferSize = fcntl(STDIN_FILENO, F_GETPIPE_SZ); }
		else { stdinPipeBufferSize = -1; }
	} else { stdinPipeBufferSize = -2; }

	int spliceStepSizeInBytes;
	// NOTE: If only one buffer size couldn't be gotten (shouldn't ever really happen, unless only one fd is a pipe), use the other one.
	if (stdinPipeBufferSize > 0) {
		if (stdoutPipeBufferSize > 0) {
			// NOTE: We use the bigger of the two pipe buffers as the splice size to save on syscalls.
			// NOTE: Don't worry about finding the smallest common multiple of the pipe buffers to keep synchronization
			// of the buffers. The buffers are ring buffers, so it doesn't matter.
			spliceStepSizeInBytes = stdinPipeBufferSize > stdoutPipeBufferSize ? stdinPipeBufferSize : stdoutPipeBufferSize;
		}
		else { spliceStepSizeInBytes = stdinPipeBufferSize; }
	} else {
		if (stdoutPipeBufferSize > 0) { spliceStepSizeInBytes = stdoutPipeBufferSize; }
		else { goto try_mmap_write_transfer; }
	}

	{
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
		// NOTE: There are 1 or 2 other possibilities, the goto is a correct reaction to all of them though.
		if (bytesSpliced == -1) { goto try_mmap_write_transfer; }

		while (true) {
			bytesSpliced = splice(STDIN_FILENO, nullptr, 
					      STDOUT_FILENO, nullptr, 
					      spliceStepSizeInBytes, 
					      SPLICE_F_MOVE | SPLICE_F_MORE);

			if (bytesSpliced == 0) { return; }

			// NOTE: If there wasn't an error in the unrolled loop iteration, there shouldn't be an error here.
			// If there does happen to be one, it's reasonable to fail the whole program, since the user should know.
			if (bytesSpliced == -1) { REPORT_ERROR_AND_EXIT("failed to transfer data: splice failed", EXIT_FAILURE); }
		}
	}

try_mmap_write_transfer:
	if (stdinPipeBufferSize != -2 && S_ISREG(status.st_mode)) {
		const char* stdinFileData = (char*)mmap(nullptr, status.st_size, PROT_READ, MAP_PRIVATE | MAP_NORESERVE | MAP_POPULATE, STDIN_FILENO, 0);
		if (stdinFileData != MAP_FAILED) {
			ssize_t amount_left = status.st_size;
			while (true) {
				ssize_t bytesWritten = write(STDOUT_FILENO, stdinFileData, amount_left);
				if (bytesWritten == -1) { REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE); }
				amount_left -= bytesWritten;
				if (amount_left == 0) {
					if (munmap((char*)stdinFileData, status.st_size)) { REPORT_ERROR_AND_EXIT("munmap for stdin file failed", EXIT_FAILURE); }
					return;
				}
			}
		}
	}

#endif

	char buffer[BUFSIZ];
	while (true) {
		sioret_t bytesRead = read(STDIN_FILENO, buffer, BUFSIZ);
		if (bytesRead == 0) { return; }
		if (bytesRead == -1) { REPORT_ERROR_AND_EXIT("failed to read from stdin", EXIT_FAILURE); }

		const char* bufferPointer = buffer;
		sioret_t bytesWritten;
		while ((bytesWritten = write(STDOUT_FILENO, bufferPointer, bytesRead)) != bytesRead) {
			if (bytesWritten == -1) { REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE); }
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
	// NOTE: Also, we don't explicitly check for NUL on the first iteration. It's implied with (digit > 9) since NUL causes failure in this case.

	// NOTE: AVOIDING SIGNED OVERFLOW, WHICH WOULD BE UNDEFINED BEHAVIOR!
	const unsigned char* input = (const unsigned char*)string_input;

	uint16_t result = input[0] - '0';
	if (result > 9) { REPORT_ERROR_AND_EXIT("invalid input for optional extra byte", EXIT_SUCCESS); }

	if (input[1] == '\0') { return result; }	// TODO: You really should have this inside of the digit > 9 check, so that the most probable route (the valid digit route) has less if's to check. That would be more efficient.
	unsigned char digit = input[1] - '0';
	if (digit > 9) { REPORT_ERROR_AND_EXIT("invalid input for optional extra byte", EXIT_SUCCESS); }
	result = result * 10 + digit;

	if (input[2] == '\0') { return result; }
	digit = input[2] - '0';
	if (digit > 9) { REPORT_ERROR_AND_EXIT("invalid input for optional extra byte", EXIT_SUCCESS); }
	result = result * 10 + digit;

	for (size_t i = 3; input[i] != '\0'; i++) {
		if (result >= 100) { REPORT_ERROR_AND_EXIT("invalid input for optional extra byte", EXIT_SUCCESS); }
		digit = input[i] - '0';
		if (digit > 9) { REPORT_ERROR_AND_EXIT("invalid input for optional extra byte", EXIT_SUCCESS); }
		result = result * 10 + digit;
	}

	if (result > 255) { REPORT_ERROR_AND_EXIT("invalid input for optional extra byte", EXIT_SUCCESS); }

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
							REPORT_ERROR_AND_EXIT("you must specify exactly one instance of either --front or --back", EXIT_SUCCESS);
						}
						flags::textAttachmentLocation = AttachmentLocation::front;
						continue;
					}
					if (std::strcmp(flagContent, "back") == 0) {
						if (flags::textAttachmentLocation != AttachmentLocation::none) {
							REPORT_ERROR_AND_EXIT("you must specify exactly one instance of either --front or --back", EXIT_SUCCESS);
						}
						flags::textAttachmentLocation = AttachmentLocation::back;
						continue;
					}
					if (std::strcmp(flagContent, "help") == 0) {
						if (argc != 2) { REPORT_ERROR_AND_EXIT("use of \"--help\" flag with other args is illegal", EXIT_SUCCESS); }
						if (write(STDOUT_FILENO, helpText, sizeof(helpText) - 1) == -1) {
							REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
						}
						std::exit(EXIT_SUCCESS);
					}
					REPORT_ERROR_AND_EXIT("one or more invalid flags specified", EXIT_SUCCESS);
				}
			case 'b':
				if (flags::extraByte != -1) { REPORT_ERROR_AND_EXIT("optional extra byte flag (\"-b\") can only be specified once", EXIT_SUCCESS); }
				i++;
				if (i == argc) { REPORT_ERROR_AND_EXIT("optional extra byte flag (\"-b\") requires a value", EXIT_SUCCESS); }
				flags::extraByte = parseByte(argv[i]);
				continue;
			default: REPORT_ERROR_AND_EXIT("one or more invalid flags specified", EXIT_SUCCESS);
			}
		}
		if (normalArgIndex != 0) { REPORT_ERROR_AND_EXIT("too many non-flag args", EXIT_SUCCESS); }
		normalArgIndex = i;
	}
	if (normalArgIndex == 0 && flags::extraByte == -1) { REPORT_ERROR_AND_EXIT("not enough args", EXIT_SUCCESS); }
	if (flags::textAttachmentLocation == AttachmentLocation::none) { REPORT_ERROR_AND_EXIT("you must specify either --front or --back", EXIT_SUCCESS); }
	return normalArgIndex;
}

// COMMAND-LINE PARSER END -----------------------------------------------------

// MAIN LOGIC START ------------------------------------------------------------

bool write_entire_buffer(int fd, const void* buffer_input, size_t size) noexcept {
	if (size == 0) { return true; }		// NOTE: Necessary because write() with count = 0 has unspecified results when fd isn't a regular file.
	const char* buffer = (const char*)buffer_input;
	while (true) {
		sioret_t bytes_written = write(fd, buffer, size);
		if (bytes_written == -1) { return false; }
		size -= bytes_written;
		if (size == 0) { return true; }
		buffer += bytes_written;
	}
}

void write_extra_byte() noexcept {
	if (flags::extraByte != -1) {
		unsigned char byte = flags::extraByte;
		if (!write_entire_buffer(STDOUT_FILENO, &byte, sizeof(byte))) { REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE); }
	}
}

void write_string_if_not_nullptr(const char* text) noexcept {
	if (text != nullptr && !write_entire_buffer(STDOUT_FILENO, text, std::strlen(text))) { REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE); }
}

void append(const char* text) noexcept {
	openFloodGates();
	write_string_if_not_nullptr(text);
	write_extra_byte();
}

void prepend(const char* text) noexcept {
	write_extra_byte();
	write_string_if_not_nullptr(text);
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
