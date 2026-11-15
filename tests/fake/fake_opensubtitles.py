"""Fake OpenSubtitles REST API (api key TESTKEY, user tolga / secret) for testing online subtitles.
   python3 fake_opensubtitles.py <port> <video file>; run Tolga with TOLGA_OPENSUBTITLES_URL=http://127.0.0.1:<port>/api/v1"""
import json, os, struct, sys, urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

VIDEO = sys.argv[2]
def movie_hash(path):
    size = os.path.getsize(path)
    h = size
    with open(path, "rb") as f:
        for off in (0, size - 65536):
            f.seek(off)
            for (v,) in struct.iter_unpack("<Q", f.read(65536)):
                h = (h + v) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h
HASH = movie_hash(VIDEO)
SRT = "1\n00:00:01,000 --> 00:00:04,000\nÇevrimiçi altyazı çalıştı\n\n2\n00:00:05,000 --> 00:00:09,000\nİkinci satır\n"

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, code, obj=None, raw=None, ctype="application/json"):
        body = raw if raw is not None else json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def check(self):
        ok = self.headers.get("Api-Key") == "TESTKEY" and self.headers.get("User-Agent", "").startswith("Tolga")
        print("REQ", self.command, self.path, "auth" if self.headers.get("Authorization") else "-", "ok" if ok else "BADHDR", flush=True)
        if not ok: self.reply(401, {"message": "invalid api key"})
        return ok
    def do_GET(self):
        if self.path.startswith("/file/"):
            print("REQ file", self.path, flush=True); return self.reply(200, raw=SRT.encode(), ctype="text/plain")
        if not self.check(): return
        u = urllib.parse.urlparse(self.path); q = urllib.parse.parse_qs(u.query)
        print("   params", q, "hash_ok" if q.get("moviehash", [""])[0] == HASH else "hash?", flush=True)
        data = []
        if q.get("moviehash", [""])[0] == HASH:
            data.append({"id": "1", "type": "subtitle", "attributes": {"language": "tr", "download_count": 1200,
                "release": "Big.Test.2010.1080p.BluRay.x264-TOLGA", "moviehash_match": True, "fps": 25.0, "upload_date": "2016-07-09T10:00:00Z", "feature_details": {"year": 2010},
                "files": [{"file_id": 111, "file_name": "Big.Test.2010.Turkish.srt"}]}})
        data.append({"id": "2", "type": "subtitle", "attributes": {"language": "en", "download_count": 99000,
            "release": "Big Test 2010 WEB-DL", "moviehash_match": False, "hearing_impaired": True, "fps": 23.976, "upload_date": "2013-11-26T10:00:00Z", "feature_details": {"year": 2010},
            "files": [{"file_id": 222, "file_name": "Big.Test.2010.English.HI.srt"}]}})
        data.append({"id": "3", "type": "subtitle", "attributes": {"language": "tr", "download_count": 50,
            "release": "Big Test 2010 DVDRip", "upload_date": "2010-11-17T00:00:00Z", "files": [{"file_id": 333, "file_name": "bt.tr.srt"}]}})
        self.reply(200, {"total_count": len(data), "data": data})
    def do_POST(self):
        if not self.check(): return
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        print("   body", body, flush=True)
        if self.path.endswith("/login"):
            if body.get("password") != "secret": return self.reply(401, {"message": "Invalid credentials"})
            return self.reply(200, {"token": "JWT123", "base_url": "localhost-not-used", "user": {"allowed_downloads": 20}})
        if self.path.endswith("/download"):
            port = self.server.server_address[1]
            return self.reply(200, {"link": f"http://127.0.0.1:{port}/file/{body['file_id']}", "file_name": "Big.Test.2010.Turkish.srt", "remaining": 19})
        self.reply(404, {"message": "no"})

HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
