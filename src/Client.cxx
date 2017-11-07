/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "Protocol.hxx"
#include "system/Error.hxx"
#include "net/log/Parser.hxx"
#include "net/log/Datagram.hxx"
#include "net/log/OneLine.hxx"
#include "net/RConnectSocket.hxx"
#include "net/UniqueSocketDescriptor.hxx"
#include "util/ConstBuffer.hxx"
#include "util/PrintException.hxx"
#include "util/StringAPI.hxx"
#include "util/StringCompare.hxx"
#include "util/ByteOrder.hxx"
#include "util/Macros.hxx"

#include <memory>

#include <sys/socket.h>
#include <stdlib.h>

struct PondDatagram {
	uint16_t id;
	PondResponseCommand command;

	struct {
		std::unique_ptr<uint8_t[]> data;
		size_t size;

		operator ConstBuffer<void>() const noexcept {
			return {data.get(), size};
		}

		std::string ToString() const {
			return std::string((const char *)data.get(), size);
		}
	} payload;
};

class PondClient {
	UniqueSocketDescriptor fd;

	uint16_t last_id = 0;

public:
	explicit PondClient(const char *server)
		:fd(ResolveConnectStreamSocket(server, 5480)) {
		fd.SetBlocking();
	}

	uint16_t MakeId() {
		return ++last_id;
	}

	void Send(uint16_t id, PondRequestCommand command,
		  ConstBuffer<void> payload=nullptr);

	void Send(uint16_t id, PondRequestCommand command,
		  StringView payload) {
		Send(id, command, payload.ToVoid());
	}

	void Send(uint16_t id, PondRequestCommand command,
		  const char *payload) {
		Send(id, command, StringView(payload));
	}

	PondDatagram Receive();
};

void
PondClient::Send(uint16_t id, PondRequestCommand command,
		 ConstBuffer<void> payload)
{
	PondHeader header;
	if (payload.size >= std::numeric_limits<decltype(header.size)>::max())
		throw std::runtime_error("Payload is too large");

	header.id = ToBE16(id);
	header.command = ToBE16(uint16_t(command));
	header.size = ToBE16(payload.size);

	struct iovec vec[] = {
		{
			.iov_base = &header,
			.iov_len = sizeof(header),
		},
		{
			.iov_base = const_cast<void *>(payload.data),
			.iov_len = payload.size,
		},
	};

	struct msghdr m = {
		.msg_name = nullptr,
		.msg_namelen = 0,
		.msg_iov = vec,
		.msg_iovlen = 1u + !payload.empty(),
		.msg_control = nullptr,
		.msg_controllen = 0,
		.msg_flags = 0,
	};

	ssize_t nbytes = sendmsg(fd.Get(), &m, 0);
	if (nbytes < 0)
		throw MakeErrno("Failed to send");

	if (size_t(nbytes) != sizeof(header) + payload.size)
		throw std::runtime_error("Short send");
}

static void
FullReceive(SocketDescriptor fd, uint8_t *buffer, size_t size)
{
	while (size > 0) {
		ssize_t nbytes = recv(fd.Get(), buffer, size, 0);
		if (nbytes < 0)
			throw MakeErrno("Failed to receive");

		if (nbytes == 0)
			throw std::runtime_error("Premature end of stream");

		buffer += nbytes;
		size -= nbytes;
	}
}

PondDatagram
PondClient::Receive()
{
	PondHeader header;
	ssize_t nbytes = recv(fd.Get(), &header, sizeof(header), 0);
	if (nbytes < 0)
		throw MakeErrno("Failed to receive");

	if (nbytes == 0)
		throw std::runtime_error("Premature end of stream");

	if (size_t(nbytes) != sizeof(header))
		throw std::runtime_error("Short receive");

	PondDatagram d;
	d.id = FromBE16(header.id);
	d.command = PondResponseCommand(FromBE16(header.command));
	d.payload.size = FromBE16(header.size);

	if (d.payload.size > 0) {
		d.payload.data.reset(new uint8_t[d.payload.size]);
		FullReceive(fd, d.payload.data.get(), d.payload.size);
	}

	return d;
}

gcc_pure
static const char *
IsFilter(const char *arg, StringView name) noexcept
{
	return StringStartsWith(arg, name) && arg[name.size] == '='
		? arg + name.size + 1
		: nullptr;
}

/**
 * Cast this #FileDescriptor to a #SocketDescriptor if it specifies a
 * socket.
 */
gcc_pure
static SocketDescriptor
CheckSocket(FileDescriptor fd)
{
	return fd.IsSocket()
		? SocketDescriptor::FromFileDescriptor(fd)
		: SocketDescriptor::Undefined();
}

/**
 * Cast this #FileDescriptor to a #SocketDescriptor if it specifies a
 * packet socket (SOCK_DGRAM or SOCK_SEQPACKET).
 */
gcc_pure
static SocketDescriptor
CheckPacketSocket(FileDescriptor fd)
{
	auto s = CheckSocket(fd);
	if (s.IsDefined() && s.IsStream())
		s.SetUndefined();
	return s;
}

static void
SendPacket(SocketDescriptor s, ConstBuffer<void> payload)
{
	struct iovec vec[] = {
		{
			.iov_base = const_cast<void *>(payload.data),
			.iov_len = payload.size,
		},
	};

	struct msghdr m = {
		.msg_name = nullptr,
		.msg_namelen = 0,
		.msg_iov = vec,
		.msg_iovlen = ARRAY_SIZE(vec),
		.msg_control = nullptr,
		.msg_controllen = 0,
		.msg_flags = 0,
	};

	sendmsg(s.Get(), &m, 0);
}

static void
Query(const char *server, ConstBuffer<const char *> args)
{
	const char *filter_site = nullptr;
	bool follow = false;

	const FileDescriptor out_fd(STDOUT_FILENO);
	auto socket = CheckPacketSocket(out_fd);

	while (!args.empty()) {
		const char *p = args.shift();
		if (auto value = IsFilter(p, "site"))
			filter_site = value;
		else if (StringIsEqual(p, "--follow"))
			follow = true;
		else
			throw "Unrecognized query argument";
	}

	PondClient client(server);
	const auto id = client.MakeId();
	client.Send(id, PondRequestCommand::QUERY);

	if (filter_site != nullptr)
		client.Send(id, PondRequestCommand::FILTER_SITE, filter_site);

	if (follow)
		client.Send(id, PondRequestCommand::FOLLOW);

	client.Send(id, PondRequestCommand::COMMIT);

	while (true) {
		const auto d = client.Receive();
		if (d.id != id)
			continue;

		switch (d.command) {
		case PondResponseCommand::NOP:
			break;

		case PondResponseCommand::ERROR:
			throw std::runtime_error(d.payload.ToString());

		case PondResponseCommand::END:
			return;

		case PondResponseCommand::LOG_RECORD:
			if (socket.IsDefined()) {
				/* if fd2 is a packet socket, send raw
				   datagrams to it */
				SendPacket(socket, d.payload);
				continue;
			}

			try {
				LogOneLine(out_fd,
					   Net::Log::ParseDatagram(d.payload.data.get(),
								   d.payload.data.get() + d.payload.size));
			} catch (Net::Log::ProtocolError) {
				fprintf(stderr, "Failed to parse log record\n");
			}

			break;
		}
	}
}

int
main(int argc, char **argv)
try {
	ConstBuffer<const char *> args(argv + 1, argc - 1);
	if (args.size < 2) {
		fprintf(stderr, "Usage: %s SERVER[:PORT] COMMAND ...\n"
			"\n"
			"Commands:\n"
			"  query [--follow] [site=VALUE]\n", argv[0]);
		return EXIT_FAILURE;
	}

	const char *const server = args.shift();
	const char *const command = args.shift();

	if (StringIsEqual(command, "query")) {
		Query(server, args);
		return EXIT_SUCCESS;
	} else {
		fprintf(stderr, "Unknown command: %s\n", command);
		return EXIT_FAILURE;
	}
} catch (const char *msg) {
	fprintf(stderr, "%s\n", msg);
	return EXIT_FAILURE;
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}
