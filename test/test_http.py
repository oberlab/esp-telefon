from http.server import HTTPServer, BaseHTTPRequestHandler

class SimpleHandler(BaseHTTPRequestHandler):
    def _handle_request(self):
        # Ausgeben der angeforderten URL
        print(self.path)
        # Antwort mit "ok"
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain; charset=utf-8')
        self.end_headers()
        self.wfile.write(b'ok')

    def do_GET(self):
        self._handle_request()

    def do_POST(self):
        self._handle_request()

    # Unterdrückt die Standard-Logging-Ausgabe
    def log_message(self, format, *args):
        pass

def run(host='0.0.0.0', port=80):
    server = HTTPServer((host, port), SimpleHandler)
    print(f'Serving on http://{host}:{port}')
    server.serve_forever()

if __name__ == '__main__':
    run()