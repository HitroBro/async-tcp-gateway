#!/usr/bin/env python3
"""
404_Team_not_Found - Automated TCP Test Client
Connects to the Gateway, sends a test payload, and validates the response.
"""

import socket
import argparse
import sys
import time

def run_test(host, port, message, expected_prefix):
    print(f"============================================================")
    print(f"[*] Starting TCP Test Client")
    print(f"[*] Target Endpoint: {host}:{port}")
    print(f"[*] Payload to send: '{message}'")
    print(f"============================================================")
    
    # 1. Create client socket
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    
    # Set a 5-second socket timeout so the test doesn't hang indefinitely if something breaks
    client_socket.settimeout(5.0)
    
    try:
        # 2. Initiate TCP Three-Way Handshake with the target (Gateway or Backend)
        print(f"[CLIENT INFO] Connecting to {host}:{port}...")
        start_time = time.time()
        client_socket.connect((host, port))
        connect_time = (time.time() - start_time) * 1000
        print(f"[CLIENT SUCCESS] Connected in {connect_time:.2f} ms!")
        
        # 3. Encode string to raw bytes and transmit
        payload_bytes = message.encode('utf-8')
        print(f"[CLIENT SEND] Sending {len(payload_bytes)} bytes over the wire...")
        client_socket.sendall(payload_bytes)
        
        # 4. Wait for and read response from the target
        print(f"[CLIENT RECV] Waiting for response...")
        response_bytes = client_socket.recv(4096)
        
        if not response_bytes:
            print("[CLIENT ERROR] Target closed the connection without sending a response!")
            return False
            
        response_str = response_bytes.decode('utf-8', errors='replace')
        print(f"[CLIENT RECV] Received {len(response_bytes)} bytes: '{response_str}'")
        
        # 5. Validate the response
        if expected_prefix in response_str and message in response_str:
            print(f"[TEST PASSED] Response matches expected echo format!")
            return True
        else:
            print(f"[TEST FAILED] Unexpected response content.")
            print(f"   Expected to contain: '{expected_prefix}' and '{message}'")
            print(f"   Actually received  : '{response_str}'")
            return False
            
    except socket.timeout:
        print("[CLIENT ERROR] Test timed out! Target accepted connection but never responded.")
        return False
    except ConnectionRefusedError:
        print(f"[CLIENT ERROR] Connection refused! Is the server/gateway running on port {port}?")
        return False
    except Exception as e:
        print(f"[CLIENT ERROR] Unexpected networking exception: {e}")
        return False
    finally:
        client_socket.close()
        print("[CLIENT INFO] Socket closed.")
        print(f"============================================================")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="TCP Test Client for Gateway Verification")
    parser.add_argument("-t", "--target", default="127.0.0.1", help="Target IP address (default: 127.0.0.1)")
    parser.add_argument("-p", "--port", type=int, default=8080, help="Target port (default: 8080 for Gateway)")
    parser.add_argument("-m", "--message", default="Hello from 404_Team_not_Found Layer 4 Proxy Test!", help="Message payload to send")
    parser.add_argument("-e", "--expected", default="[ECHO FROM BACKEND]:", help="Expected string prefix in response")
    
    args = parser.parse_args()
    
    success = run_test(args.target, args.port, args.message, args.expected)
    sys.exit(0 if success else 1)