import socket
import struct
import time
from enum import Enum

class DservType(Enum):
    BYTE = 0
    STRING = 1
    FLOAT = 2
    DOUBLE = 3
    SHORT = 4
    INT = 5

def binary_dpoint(varname, datatype, data):
    """
    Formats a 128-byte binary string as fixed length dpoint

    Parameters:
    - varname: The string to include (char string[size]).
    - datatype: An integer representing the datatype (uint32_t).
    - data: The binary data (unsigned char data[datalen]).

    Returns:
    - A 128-byte binary string.
    """
    # Ensure varname fits within uint16_t size limits
    if len(varname) > 65535:
        raise ValueError("varname exceeds the maximum allowed size (65535 bytes)")

    if datatype == DservType.STRING.value:
        byte_data = bytes(data, "utf-8")
    elif datatype == DservType.INT.value:
        if type(data) == list:
            n = len(data)
            byte_data = struct.pack(f"<{'I'*n}", *data)
        else:
            byte_data = struct.pack('<I', data)
    elif datatype == DservType.SHORT.value:
        if type(data) == list:
            n = len(data)
            byte_data = struct.pack(f"<{'H'*n}", *data)
        else:
            byte_data = struct.pack('<H', data)
    elif datatype == DservType.BYTE.value:
        if type(data) == list:
            n = len(data)
            byte_data = struct.pack(f"<{'B'*n}", *data)
        else:
            byte_data = struct.pack('<B', data)

    elif datatype == DservType.FLOAT.value:
        if type(data) == list:
            n = len(data)
            byte_data = struct.pack(f"<{'f'*n}", *data)
        else:
            byte_data = struct.pack('<f', data)
    elif datatype == DservType.DOUBLE.value:
        if type(data) == list:
            n = len(data)
            byte_data = struct.pack(f"<{'d'*n}", *data)
        else:
            byte_data = struct.pack('<d', data)


    # Timestamp (uint64_t) set by server
    timestamp = 0
            
    # Ensure data fits within 128 bytes after padding
    size = len(varname)
    datalen = len(byte_data)
    prefix_size = 1 # for the '>' character
    if prefix_size + 2 + size + 8 + 4 + 4 + datalen > 128:
        raise ValueError("The total data exceeds the 128-byte limit")

    # Calculate padding to ensure the output is 128 bytes
    message_size = prefix_size + 2 + size + 8 + 4 + 4 + datalen
    padding = 128 - message_size

    if padding < 0:
        raise ValueError("Padding calculation error: data exceeds 128-byte limit")
            
    # Pack the data
    binary_string = struct.pack(
        f"<cH{size}sQII{datalen}s{padding}x",
        b'>',                      # prefix character '>'
        size,                      # uint16_t size
        varname.encode(),          # char string[size]
        timestamp,                 # uint64_t timestamp
        datatype,                  # uint32_t datatype
        datalen,                   # uint32_t datalen
        byte_data                  # unsigned char data[datalen]
    )

    return binary_string


def send_to_dataserver(socket, varname, datatype, data):
    """
    create a fixed length datapoint and send to socket

    Parameters:
    - socket: connected socket
    - varname: The string to include (char string[size]).
    - datatype: An integer representing the datatype (uint32_t).
    - data: The binary data (unsigned char data[datalen]).
    """

    dpoint = binary_dpoint(varname, datatype, data)

    # Create a TCP socket
    try:
        # Send the binary data
        socket.sendall(dpoint)
            
    except Exception as e:
        print(f"An error occurred: {e}")


if __name__ == "__main__":            
    server_ip = "127.0.0.1"
    server_port = 4620
    
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client_socket.connect((server_ip, server_port))

    varname = "test/string"
    datatype = DservType.STRING.value
    data = "val"
    send_to_dataserver(client_socket, varname, datatype, data)

    varname = "test/int"
    datatype = DservType.INT.value
    data = 2025
    send_to_dataserver(client_socket, varname, datatype, data)

    varname = "test/shorts"
    datatype = DservType.SHORT.value
    data = [100, 200]
    send_to_dataserver(client_socket, varname, datatype, data)

    varname = "test/floats"
    datatype = DservType.FLOAT.value
    data = [100, 200, 300]
    send_to_dataserver(client_socket, varname, datatype, data)

    varname = "test/double"
    datatype = DservType.FLOAT.value
    data = 3.1415926
    send_to_dataserver(client_socket, varname, datatype, data)
