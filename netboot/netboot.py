#!/usr/bin/env python3
# Triforce Netfirm Toolbox, put into the public domain.
# Please attribute properly, but only if you want.
import socket
import struct
import zlib
from Crypto.Cipher import DES
from contextlib import contextmanager
from enum import Enum
from typing import Callable, Generator, List, Optional, cast

from netboot.log import log


class NetDimmException(Exception):
    pass


class TargetEnum(Enum):
    TARGET_CHIHIRO = "chihiro"
    TARGET_NAOMI = "naomi"
    TARGET_TRIFORCE = "triforce"


class TargetVersionEnum(Enum):
    TARGET_VERSION_UNKNOWN = "UNKNOWN"
    TARGET_VERSION_1_07 = "1.07"
    TARGET_VERSION_2_03 = "2.03"
    TARGET_VERSION_2_15 = "2.15"
    TARGET_VERSION_3_01 = "3.01"
    TARGET_VERSION_4_01 = "4.01"
    TARGET_VERSION_4_02 = "4.02"


class NetDimmInfo:
    def __init__(self, current_game_crc: int, memory_size: int, firmware_version: TargetVersionEnum, available_game_memory: int) -> None:
        self.current_game_crc = current_game_crc
        self.memory_size = memory_size
        self.firmware_version = firmware_version
        self.available_game_memory = available_game_memory


class NetDimmPacket:
    def __init__(self, pktid: int, flags: int, data: bytes = b'') -> None:
        self.pktid = pktid
        self.flags = flags
        self.data = data

    @property
    def length(self) -> int:
        return len(self.data)


class NetDimm:
    @staticmethod
    def crc(data: bytes) -> int:
        crc: int = 0
        crc = zlib.crc32(data, crc)
        return (~crc) & 0xFFFFFFFF

    def __init__(self, ip: str, target: Optional[TargetEnum] = None, version: Optional[TargetVersionEnum] = None, quiet: bool = False) -> None:
        self.ip: str = ip
        self.sock: Optional[socket.socket] = None
        self.quiet: bool = quiet
        self.target: TargetEnum = target or TargetEnum.TARGET_NAOMI
        self.version: TargetVersionEnum = version or TargetVersionEnum.TARGET_VERSION_UNKNOWN

    def __repr__(self) -> str:
        return f"NetDimm(ip={repr(self.ip)}, target={repr(self.target)}, version={repr(self.version)})"

    def info(self) -> NetDimmInfo:
        with self.__connection():
            # Ask for DIMM firmware info and such.
            return self.__get_information()

    def send(self, data: bytes, key: Optional[bytes] = None, progress_callback: Optional[Callable[[int, int], None]] = None) -> None:
        with self.__connection():
            # First, signal back to calling code that we've started
            if progress_callback:
                progress_callback(0, len(data))

            # Reboot and display "now loading..." on the cabinet screen
            self.__set_host_mode(1)

            if key:
                # Send the key that we're going to use to encrypt
                self.__set_key_code(key)
            else:
                # disable encryption by setting magic zero-key
                self.__set_key_code(b"\x00" * 8)

            # uploads file. Also sets "dimm information" (file length and crc32)
            self.__upload_file(data, key, progress_callback or (lambda _cur, _tot: None))

    def reboot(self) -> None:
        with self.__connection():
            # restart host, this wil boot into game
            self.__restart()

            # set time limit to 10 minutes.
            self.__set_time_limit(10)

            if self.target == TargetEnum.TARGET_TRIFORCE:
                self.__patch_boot_id_check()

    def __print(self, string: str, newline: bool = True) -> None:
        if not self.quiet:
            log(string, newline=newline)

    def __read(self, num: int) -> bytes:
        if self.sock is None:
            raise NetDimmException("Not connected to NetDimm")

        # a function to receive a number of bytes with hard blocking
        res: List[bytes] = []
        left: int = num

        while left > 0:
            ret = self.sock.recv(left)
            left -= len(ret)
            res.append(ret)

        return b"".join(res)

    @contextmanager
    def __connection(self) -> Generator[None, None, None]:
        # connect to the Triforce. Port is tcp/10703.
        # note that this port is only open on
        #       - all Type-3 triforces,
        #       - pre-type3 triforces jumpered to satellite mode.
        # - it *should* work on naomi and chihiro, but due to lack of hardware, i didn't try.
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(1)
            self.sock.connect((self.ip, 10703))
            self.sock.settimeout(10)
        except Exception as e:
            raise NetDimmException("Could not connect to NetDimm") from e

        try:
            self.__startup()

            yield
        finally:
            self.sock.close()
            self.sock = None

    # Both requests and responses follow this header with length data bytes
    # after. Some have the ability to send/receive variable length (like send/recv
    # dimm packets) and some require a specific length or they do not return.
    #
    # Header words are packed as little-endian bytes and are as such: AABBCCCC
    # AA -   Packet type. Any of 256 values, but in practice most are unrecognized.
    # BB -   Seems to be some sort of flags and flow control. Packets have been observed
    #        with 00, 80 and 81 in practice. The bottom bit signifies, as best as I can
    #        tell, that the host intends to send more of the same packet type. It set to
    #        0 for dimm send requests except for the last packet. The top bit I have not
    #        figured out.
    # CCCC - Length of the data in bytes that follows this header, not including the 4
    #        header bytes.
    def __send_packet(self, packet: NetDimmPacket) -> None:
        if self.sock is None:
            raise NetDimmException("Not connected to NetDimm")
        self.sock.send(
            struct.pack(
                "<I",
                (
                    ((packet.pktid & 0xFF) << 24) |  # noqa: W504
                    ((packet.flags & 0xFF) << 16) |  # noqa: W504
                    (packet.length & 0xFFFF)
                ),
            ) + packet.data
        )

    def __recv_packet(self) -> NetDimmPacket:
        # First read the header to get the packet length.
        header = self.__read(4)

        # Construct a structure to represent this packet, minus any optional data.
        headerbytes = struct.unpack("<I", header)[0]
        packet = NetDimmPacket(
            (headerbytes >> 24) & 0xFF,
            (headerbytes >> 16) & 0xFF,
        )
        length = headerbytes & 0xFFFF

        # Read optional data.
        if length > 0:
            data = self.__read(length)
            packet.data = data

        # Return the parsed packet.
        return packet

    def __startup(self) -> None:
        # This is mapped to a NOOP packet. At least in older versions of net dimm. I don't
        # know why transfergame.exe sends this, maybe older versions of net dimm firmware need it?
        self.__send_packet(NetDimmPacket(0x01, 0x00))

    def __host_poke4(self, addr: int, data: int) -> None:
        self.__send_packet(NetDimmPacket(0x11, 0x00, struct.pack("<III", addr, 0, data)))

    def __exchange_host_mode(self, mask_bits: int, set_bits: int) -> int:
        self.__send_packet(NetDimmPacket(0x07, 0x00, struct.pack("<I", ((mask_bits & 0xFF) << 8) | (set_bits & 0xFF))))

        # Set mode returns the resulting mode, after the original mode is or'd with "set_bits"
        # and and'd with "mask_bits". I guess this allows you to see the final mode with your
        # additions and subtractions since it might not be what you expect. It also allows you
        # to query the current mode by sending a packet with mask bits 0xff and set bits 0x0.
        response = self.__recv_packet()
        if response.pktid != 0x07:
            raise NetDimmException("Unexpected data returned from set host mode packet!")
        if response.length != 4:
            raise NetDimmException("Unexpected data length returned from set host mode packet!")

        # The top 3 bytes are not set to anything, only the bottom byte is set to the combined mode.
        return cast(int, struct.unpack("<I", response.data)[0] & 0xFF)

    def __set_host_mode(self, mode: int) -> None:
        self.__exchange_host_mode(0, mode)

    def __get_host_mode(self) -> int:
        # The following modes have been observed:
        #
        # 0 - System is in "CHECKING NETWORK"/"CHECKING MEMORY"/running game mode.
        # 1 - System was requested to display "NOW LOADING..." and is rebooting into that mode.
        # 2 - System is in "NOW LOADING..." mode but no transfer has been initiated.
        # 10 - System is in "NOW LOADING..." mode and a transfer has been initiated, rebooting naomi before continuing.
        # 20 - System is in "NOW LIADING..." mode and a transfer is continuing.
        return self.__exchange_host_mode(0xFF, 0)

    def __exchange_dimm_mode(self, mask_bits: int, set_bits: int) -> int:
        self.__send_packet(NetDimmPacket(0x08, 0x00, struct.pack("<I", ((mask_bits & 0xFF) << 8) | (set_bits & 0xFF))))

        # Set mode returns the resulting mode, after the original mode is or'd with "set_bits"
        # and and'd with "mask_bits". I guess this allows you to see the final mode with your
        # additions and subtractions since it might not be what you expect. It also allows you
        # to query the current mode by sending a packet with mask bits 0xff and set bits 0x0.
        response = self.__recv_packet()
        if response.pktid != 0x08:
            raise NetDimmException("Unexpected data returned from set dimm mode packet!")
        if response.length != 4:
            raise NetDimmException("Unexpected data length returned from set dimm mode packet!")

        # The top 3 bytes are not set to anything, only the bottom byte is set to the combined mode.
        return cast(int, struct.unpack("<I", response.data)[0] & 0xFF)

    def __set_dimm_mode(self, mode: int) -> None:
        # Absolutely no idea what this does. You can set any mode and the below __get_dimm_mode
        # will return the same value, and it survives soft reboots as well as hard power cycles.
        # It seems to have no effect on the system.
        self.__exchange_dimm_mode(0, mode)

    def __get_dimm_mode(self) -> int:
        return self.__exchange_dimm_mode(0xFF, 0)

    def __set_key_code(self, keydata: bytes) -> None:
        if len(keydata) != 8:
            raise NetDimmException("Key code must by 8 bytes in length")
        self.__send_packet(NetDimmPacket(0x7F, 0x00, keydata))

    def __upload(self, sequence: int, addr: int, data: bytes, last_chunk: bool) -> None:
        # Upload a chunk of data to the DIMM address "addr". The sequence seems to
        # be just a marking for what number packet this is. The last chunk flag is
        # an indicator for whether this is the last packet or not and gets used to
        # set flag bits. If there is no additional data (the length portion is set
        # to 0xA), the packet will be rejected. The net dimm does not seem to parse
        # the sequence number in fw 3.17 but transfergame.exe sends it. The last
        # short does not seem to do anything and does not appear to even be parsed.
        self.__send_packet(NetDimmPacket(0x04, 0x81 if last_chunk else 0x80, struct.pack("<IIH", sequence, addr, 0) + data))

    def __download(self, addr: int, size: int) -> bytes:
        self.__send_packet(NetDimmPacket(0x05, 0x00, struct.pack("<II", addr, size)))

        # Read the data back. The flags byte will be 0x80 if the requested data size was
        # too big, and 0x81 if all of the data was able to be returned. It looks like at
        # least for 3.17 this limit is 8192. However, the net dimm will continue sending
        # packets until all data has been received.
        data = b""

        while True:
            chunk = self.__recv_packet()

            if chunk.pktid != 0x04:
                # For some reason, this is wrong for 3.17?
                raise NetDimmException("Unexpected data returned from download packet!")
            if chunk.length <= 10:
                raise NetDimmException("Unexpected data length returned from download packet!")

            # The sequence is set to 1 for the first packet and then incremented for each
            # subsequent packet until the end of data flag is received. It can be safely
            # discarded in practice. I guess there might be some reason to reassemble with
            # the sequences in the correct order, but as of net dimm 3.17 the firware will
            # always send things back in order one packet at a time.
            _chunksequence, chunkaddr, _ = struct.unpack("<IIH", chunk.data[0:10])
            data += chunk.data[10:]

            if chunk.flags & 0x1 != 0:
                # We finished!
                return data

    def __get_information(self) -> NetDimmInfo:
        self.__send_packet(NetDimmPacket(0x18, 0x00))

        # Get the info from the DIMM.
        response = self.__recv_packet()
        if response.pktid != 0x18:
            raise NetDimmException("Unexpected data returned from get info packet!")
        if response.length != 12:
            raise NetDimmException("Unexpected data length returned from get info packet!")

        # I don't know what the second integer half represents. It is "0xC"
        # on both NetDimms I've tried this on. There's no use of it in transfergame.
        # At least on firmware 3.17 this is hardcoded to 0xC so it might be the
        # protocol version?
        unknown, version, game_memory, dimm_memory, crc = struct.unpack("<HHHHI", response.data)

        # Extract version and size string.
        version_str = f"{(version >> 8) & 0xFF}.{(version & 0xFF):02}"
        try:
            firmware_version = TargetVersionEnum(version_str)
        except ValueError:
            firmware_version = TargetVersionEnum.TARGET_VERSION_UNKNOWN

        return NetDimmInfo(
            current_game_crc=crc,
            memory_size=dimm_memory,
            firmware_version=firmware_version,
            available_game_memory=game_memory << 20,
        )

    def __set_information(self, crc: int, length: int) -> None:
        self.__send_packet(NetDimmPacket(0x19, 0x00, struct.pack("<III", crc & 0xFFFFFFFF, length, 0)))

    def __upload_file(self, data: bytes, key: Optional[bytes], progress_callback: Optional[Callable[[int, int], None]]) -> None:
        # upload a file into DIMM memory, and optionally encrypt for the given key.
        # note that the re-encryption is obsoleted by just setting a zero-key, which
        # is a magic to disable the decryption.
        crc: int = 0
        addr: int = 0
        total: int = len(data)
        des = DES.new(key[::-1], DES.MODE_ECB) if key else None

        def __encrypt(chunk: bytes) -> bytes:
            if des is None:
                return chunk
            return des.encrypt(chunk[::-1])[::-1]

        sequence = 1
        while addr < total:
            self.__print("%08x %d%%\r" % (addr, int(float(addr * 100) / float(total))), newline=False)
            if progress_callback:
                progress_callback(addr, total)

            current = data[addr:(addr + 0x8000)]
            curlen = len(current)
            last_packet = addr + curlen == total

            current = __encrypt(current)
            self.__upload(sequence, addr, current, last_packet)
            crc = zlib.crc32(current, crc)
            addr += curlen
            sequence += 1

        crc = (~crc) & 0xFFFFFFFF
        self.__print("length: %08x" % addr)
        self.__set_information(crc, addr)

    def __close(self) -> None:
        # Request the net dimm to close the connection and stop listening for additional connections.
        # Unclear why you would want to use this since you have to reboot after doing this.
        self.__send_packet(NetDimmPacket(0x09, 0x00))

    def __restart(self) -> None:
        self.__send_packet(NetDimmPacket(0x0A, 0x00))

    def __set_time_limit(self, minutes: int) -> None:
        # According to the 3.17 firmware, this looks to be minutes? The value is checked to be
        # less than 10, and if so multiplied by 60,000. If not, the default value of 60,000 is used.
        self.__send_packet(NetDimmPacket(0x17, 0x00, struct.pack("<I", minutes)))

    def __patch_boot_id_check(self) -> None:
        # this essentially removes a region check, and is triforce-specific; It's also segaboot-version specific.
        # - look for string: "CLogo::CheckBootId: skipped."
        # - binary-search for lower 16bit of address
        addr = {
            TargetVersionEnum.TARGET_VERSION_1_07: 0x8000d8a0,
            TargetVersionEnum.TARGET_VERSION_2_03: 0x8000CC6C,
            TargetVersionEnum.TARGET_VERSION_2_15: 0x8000CC6C,
            TargetVersionEnum.TARGET_VERSION_3_01: 0x8000dc5c,
        }.get(self.version, None)

        if addr is None:
            # We can't do anything here.
            return

        if self.version == TargetVersionEnum.TARGET_VERSION_3_01:
            self.__host_poke4(addr + 0, 0x4800001C)
        else:
            self.__host_poke4(addr + 0, 0x4e800020)
            self.__host_poke4(addr + 4, 0x38600000)
            self.__host_poke4(addr + 8, 0x4e800020)
            # TODO: This was originally + 0 in the original script but it is suspect
            # given the address we were poking was already overwritten. I can't check
            # this but maybe somebody else can?
            self.__host_poke4(addr + 12, 0x60000000)
