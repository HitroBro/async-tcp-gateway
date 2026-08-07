#!/usr/bin/env python3
"""
404_Team_not_Found - Mock Backend Server
Simulates a TCP service for end-to-end gateway validation.
"""

import socket
import threading
import argparse
import time
import sys

def handle_client(client_socket, client_address, mode, delay_seconds):
    """
    Worker function running in a separate thread for each connected socket.
    """
    print(f"[BACKEND INFO] Accepted connection from {client_address[0]}:{client_address[1]}")
    
    try:
        while True:
            # Block and wait for data from the gateway/client
            data = client_socket.recv(4096)
            if not data:
                print(f"[BACKEND INFO] Connection closed by remote peer {client_address[0]}:{client_address[1]}")
                break
                
            print(f"[BACKEND RECV] Received {len(data)} bytes: {data.decode('utf-8', errors='replace')}")
            
            # Behavior 1: Crash Mode (Simulate unexpected server failure)
            if mode == "crash":
                print("[BACKEND CRASH] Simulating sudden server crash! Closing socket immediately.")
                # Closing without sending data or a proper FIN sequence simulates a dropped backend
                break
                
            # Behavior 2: Delay Mode (Simulate a slow backend service)
            if mode == "delay":
                print(f"[BACKEND DELAY] Sleeping for {delay_seconds} seconds before responding...")
                time.sleep(delay_seconds)
                
            # Behavior 3: Echo Mode (Default normal operation)
            response = f"[ECHO FROM BACKEND]: ".encode('utf-8') + data
            client_socket.sendall(response)
            print(f"[BACKEND SENT] Echoed {len(response)} bytes back to {client_address[0]}:{client_address[1]}")
            
    except Exception as e:
        print(f"[BACKEND ERROR] Error handling client {client_address}: {e}")
    finally:
        client_socket.close()
        print(f"[BACKEND INFO] Cleaned up socket for {client_address[0]}:{client_address[1]}")

def start_server(port, mode, delay_seconds):
    # 1. Create a TCP/IPv4 socket
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    
    # 2. Prevent "Address already in use" errors during rapid restart testing
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        # 3. Bind to all local interfaces on the specified port
        server_socket.bind(("0.0.0.0", port))
        
        # 4. Put the socket into listening mode (queue up to 50 connection requests)
        server_socket.listen(50)
        print(f"============================================================")
        print(f"[*] Mock Backend Server running on port {port}")
        print(f"[*] Operational Mode: {mode.upper()}" + (f" ({delay_seconds}s delay)" if mode == "delay" else ""))
        print(f"[*] Waiting for incoming connections from Gateway...")
        print(f"============================================================")
        
        while True:
            # Blocking accept call; returns a new socket and client address
            client_sock, client_addr = server_socket.accept()
            
            # Spawn a new thread to handle this connection concurrently
            client_thread = threading.Thread(
                target=handle_client,
                args=(client_sock, client_addr, mode, delay_seconds),
                daemon=True
            )
            client_thread.start()
            
    except KeyboardInterrupt:
        print("\n[BACKEND INFO] Server shutting down due to keyboard interrupt.")
    except Exception as e:
        print(f"[BACKEND FATAL] Server exception: {e}")
    finally:
        server_socket.close()
        sys.exit(0)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Mock TCP Backend Server for Gateway Testing")
    parser.add_argument("-p", "--port", type=int, default=9001, help="Port to listen on (default: 9001)")
    parser.add_argument("-m", "--mode", choices=["echo", "delay", "crash"], default="echo", help="Server behavior mode")
    parser.add_argument("-d", "--delay", type=float, default=2.0, help="Delay in seconds for 'delay' mode")
    
    args = parser.parse_args()
    start_server(args.port, args.mode, args.delay)