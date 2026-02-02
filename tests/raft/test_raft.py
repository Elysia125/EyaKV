import os
import subprocess
import socket
import struct
import time
import shutil
import signal
import sys

# Constants
EYAKV_SERVER = r"e:\TinyKV\build\bin\eyakv_server.exe"
BASE_DIR = r"e:\TinyKV\build\test_cluster"

class Node:
    def __init__(self, node_id, kv_port, raft_port):
        self.node_id = node_id
        self.kv_port = kv_port
        self.raft_port = raft_port
        self.dir = os.path.join(BASE_DIR, f"node_{node_id}")
        self.process = None
        self.conf_path = os.path.join(self.dir, "eyakv.conf")
        
        # Paths
        self.data_dir = os.path.join(self.dir, "data")
        self.wal_dir = os.path.join(self.dir, "wal")
        self.log_dir = os.path.join(self.dir, "logs")
        
        if os.path.exists(self.dir):
            shutil.rmtree(self.dir)
        os.makedirs(self.data_dir, exist_ok=True)
        os.makedirs(self.wal_dir, exist_ok=True)
        os.makedirs(self.log_dir, exist_ok=True)
        
    def write_config(self):
        config = f"""
ip=127.0.0.1
port={self.kv_port}
raft_port={self.raft_port}
raft_trust_ip=127.0.0.1
data_dir={self.data_dir}
wal_dir={self.wal_dir}
log_dir={self.log_dir}
log_level=0
password=root
"""
        with open(self.conf_path, "w") as f:
            f.write(config)
            
    def start(self):
        env = os.environ.copy()
        env["EYAKV_CONFIG_PATH"] = self.conf_path
        log_file = open(os.path.join(self.log_dir, "stdout.log"), "w")
        self.process = subprocess.Popen(
            [EYAKV_SERVER],
            env=env,
            cwd=os.path.dirname(EYAKV_SERVER),
            stdout=log_file,
            stderr=subprocess.STDOUT
        )
        print(f"Node {self.node_id} started (KV:{self.kv_port}, Raft:{self.raft_port})")

    def stop(self):
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except:
                self.process.kill()
            self.process = None

def serialize_string(s):
    data = s.encode('utf-8')
    return struct.pack('>I', len(data)) + data

def deserialize_string(data, offset):
    length = struct.unpack('>I', data[offset:offset+4])[0]
    offset += 4
    s = data[offset:offset+length].decode('utf-8')
    offset += length
    return s, offset

def create_request(request_id, req_type, command, auth_key=""):
    body = serialize_string(request_id)
    body += struct.pack('B', req_type)
    body += serialize_string(command)
    body += serialize_string(auth_key)
    
    magic = 0xEA1314
    version = 1
    length = len(body)
    header = struct.pack('>IBI', magic, version, length)
    return header + body

def recv_all(s, n):
    data = b''
    while len(data) < n:
        packet = s.recv(n - len(data))
        if not packet:
            return None
        data += packet
    return data

def send_command(port, command):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect(('127.0.0.1', port))
        
        # 1. Skip welcome state (9-byte header)
        header_raw = recv_all(s, 9)
        if not header_raw: return None
        magic, version, body_len = struct.unpack('>IBI', header_raw)
        if magic != 0xEA1314:
            print(f"Invalid magic: {hex(magic)}")
            return None
        recv_all(s, body_len)
        
        # 2. Auth
        auth_req = create_request("auth_id", 1, "root") # AUTH = 1
        s.sendall(auth_req)
        
        header_raw = recv_all(s, 9)
        if not header_raw: return None
        magic, version, body_len = struct.unpack('>IBI', header_raw)
        resp_data = recv_all(s, body_len)
        
        # Deserialize to get auth_key
        offset = 0
        req_id, offset = deserialize_string(resp_data, offset)
        code = struct.unpack('>i', resp_data[offset:offset+4])[0]
        offset += 4
        if code != 1:
            print(f"Auth failed for port {port}, code={code}")
            return None
        
        type_idx = resp_data[offset]
        offset += 1
        auth_key = ""
        if type_idx == 1: # String
            auth_key, offset = deserialize_string(resp_data, offset)
        
        # 3. Send Command
        cmd_req = create_request("cmd_id", 2, command, auth_key) # COMMAND = 2
        s.sendall(cmd_req)
        
        header_raw = recv_all(s, 9)
        if not header_raw: return None
        magic, version, body_len = struct.unpack('>IBI', header_raw)
        resp_data = recv_all(s, body_len)
        
        s.close()
        return resp_data
    except Exception as e:
        print(f"Error communicating with port {port}: {e}")
        return None

def parse_response(data):
    if not data: return "No Data"
    try:
        offset = 0
        req_id, offset = deserialize_string(data, offset)
        code = struct.unpack('>i', data[offset:offset+4])[0]
        offset += 4
        type_idx = data[offset]
        offset += 1
        
        res_val = "N/A"
        if type_idx == 1: # String
            res_val, offset = deserialize_string(data, offset)
        elif type_idx == 0: # Monostate
            res_val = "null"
        elif type_idx == 2: # Vector<String>
            count = struct.unpack('>I', data[offset:offset+4])[0]
            offset += 4
            vals = []
            for _ in range(count):
                v, offset = deserialize_string(data, offset)
                vals.append(v)
            res_val = str(vals)
            
        err_msg, offset = deserialize_string(data, offset)
        return {"code": code, "val": res_val, "err": err_msg}
    except:
        return f"Parse Error: {data.hex()}"

def main():
    nodes = [
        Node(0, 5210, 5211),
        Node(1, 5220, 5221),
        Node(2, 5230, 5231)
    ]
    
    try:
        for n in nodes:
            n.write_config()
            n.start()
            
        print("Waiting for nodes to start...")
        time.sleep(3)
        
        print("\n--- Step 1: Check initial status ---")
        for i, n in enumerate(nodes):
            resp = send_command(n.kv_port, "get_status")
            print(f"Node {i} status: {parse_response(resp)}")

        print("\n--- Step 2: Form cluster (Node 1, 2 join Node 0) ---")
        print("Node 1 -> set_master 127.0.0.1 5211")
        # Note: set_master uses RAFT port
        send_command(nodes[1].kv_port, "set_master 127.0.0.1 5211")
        
        print("Node 2 -> set_master 127.0.0.1 5211")
        send_command(nodes[2].kv_port, "set_master 127.0.0.1 5211")
        
        time.sleep(5) # Wait for cluster stable
        
        print("\n--- Step 3: Check status again ---")
        for i, n in enumerate(nodes):
            resp = send_command(n.kv_port, "get_status")
            print(f"Node {i} status: {parse_response(resp)}")

        print("\n--- Step 4: KV Replication Test ---")
        print("Sending 'set mykey hello_raft' to Node 0...")
        resp = send_command(nodes[0].kv_port, "set mykey hello_raft")
        print(f"Set Response: {parse_response(resp)}")
        
        time.sleep(2)
        
        print("Reading 'get mykey' from all nodes...")
        for i, n in enumerate(nodes):
            resp = send_command(n.kv_port, "get mykey")
            print(f"Node {i} 'get mykey': {parse_response(resp)}")

    finally:
        print("\nCleaning up...")
        for n in nodes:
            n.stop()

if __name__ == "__main__":
    main()
