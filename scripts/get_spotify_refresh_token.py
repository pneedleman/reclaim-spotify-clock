#!/usr/bin/env python3
"""
Spotify Refresh Token Generator
Reclaim Spotify Bedside Clock

Usage:
    python3 get_spotify_refresh_token.py <CLIENT_ID> <CLIENT_SECRET>

Helper tool to authorize your Spotify Developer App and retrieve
a long-lived SECRET_SPOTIFY_REFRESH_TOKEN for your ESP32 clock.
"""

import sys
import urllib.parse
import webbrowser
from http.server import HTTPServer, BaseHTTPRequestHandler
import requests

PORT = 8888
REDIRECT_URI = f"http://127.0.0.1:{PORT}/callback"
AUTH_CODE = None

class CallbackHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        global AUTH_CODE
        query = urllib.parse.urlparse(self.path).query
        params = urllib.parse.parse_qs(query)
        
        self.send_response(200)
        self.send_header('Content-type', 'text/html')
        self.end_headers()
        
        if 'code' in params:
            AUTH_CODE = params['code'][0]
            self.wfile.write(b"<h1>Success!</h1><p>Authorization code received. You can close this tab and return to the terminal.</p>")
        else:
            self.wfile.write(b"<h1>Error</h1><p>Authorization code not found in request.</p>")
            
    def log_message(self, format, *args):
        pass # Suppress HTTP server output

def get_tokens(client_id, client_secret):
    global AUTH_CODE
    
    scopes = "user-modify-playback-state user-read-playback-state playlist-read-private playlist-read-collaborative"
    auth_url = (
        "https://accounts.spotify.com/authorize?" +
        urllib.parse.urlencode({
            "client_id": client_id,
            "response_type": "code",
            "redirect_uri": REDIRECT_URI,
            "scope": scopes
        })
    )
    
    print("\n[Auth] Opening browser to Spotify authorization page...")
    print(f"If browser does not open automatically, visit:\n{auth_url}\n")
    webbrowser.open(auth_url)
    
    server = HTTPServer(('127.0.0.1', PORT), CallbackHandler)
    while AUTH_CODE is None:
        server.handle_request()
        
    print("[Auth] Callback code received successfully!")
    
    token_url = "https://accounts.spotify.com/api/token"
    payload = {
        "grant_type": "authorization_code",
        "code": AUTH_CODE,
        "redirect_uri": REDIRECT_URI,
        "client_id": client_id,
        "client_secret": client_secret
    }
    
    response = requests.post(token_url, data=payload)
    data = response.json()
    
    if "refresh_token" in data:
        print("\n" + "="*60)
        print("🎉 SUCCESS! Copy your Spotify Refresh Token below:")
        print("="*60)
        print(f"\nSPOTIFY_REFRESH_TOKEN:\n{data['refresh_token']}\n")
        print("="*60)
        print("Paste this value into #define SECRET_SPOTIFY_REFRESH_TOKEN inside src/secrets.h!\n")
    else:
        print("\n❌ Error retrieving refresh token:")
        print(data)

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 get_spotify_refresh_token.py <SPOTIFY_CLIENT_ID> <SPOTIFY_CLIENT_SECRET>")
        sys.exit(1)
        
    client_id = sys.argv[1]
    client_secret = sys.argv[2]
    get_tokens(client_id, client_secret)
