#if HAS_TFT && defined(USE_KEYBOARD_VIEW) && (defined(ARCH_PORTDUINO) || defined(ARDUINO_ARCH_ESP32))

#include "comms/UploadServer.h"
#include "storage/IStorage.h"
#include "util/ILog.h"
#include <Arduino.h>
#include <WiFi.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#if defined(ARCH_PORTDUINO)
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <random>
#else
#include "esp_random.h"
#endif

namespace
{

constexpr size_t c_headerMax = 2048;      // bytes of a request before its body
constexpr size_t c_chunk = 4096;          // bytes written to the card at once
constexpr size_t c_pathMax = 200;
constexpr uint32_t c_headerTimeout = 10000; // ms without data
constexpr uint32_t c_bodyTimeout = 15000;
constexpr uint32_t c_drainTimeout = 3000;
constexpr uint32_t c_headerTotal = 20000; // ms a request may take to send its headers, however it trickles
constexpr uint32_t c_drainTotal = 8000;   // ms and
constexpr uint64_t c_drainMax = 1 << 20;  // bytes of a refused body that are read at all
constexpr uint32_t c_wrongCodes = 20;     // then the server stops listening to codes

const char c_page[] = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>M9 files</title>
<style>body{font:16px system-ui,sans-serif;max-width:34em;margin:1em auto;padding:0 1em;background:#0b0f14;color:#eef2f6}
input,button{font:inherit;padding:.4em;margin:.2em 0;border-radius:6px;border:1px solid #5a6878;background:#1e2630;color:inherit}
input{width:100%;box-sizing:border-box}button{background:#ffb02e;color:#000;border:0;padding:.6em 1.4em}
label{display:block;margin-top:.8em;color:#9fb0c3}progress{width:100%}#log{white-space:pre-wrap;font-size:14px;margin-top:1em}</style>
<h2>Send files to the radio</h2>
<label>Code shown on the radio<input id=c inputmode=numeric maxlength=6></label>
<label>Folder on the SD card<input id=d value="/maps"></label>
<label>Files<input id=f type=file multiple></label>
<label>or a whole folder<input id=g type=file webkitdirectory></label>
<p><button id=b>Send</button></p><progress id=p value=0 max=1></progress><div id=log></div>
<p style="color:#9fb0c3;font-size:14px">A .pmtiles map sent to /maps gets its own folder and shows up as a map style.</p>
<script>
const $=i=>document.getElementById(i),L=$('log');
$('c').value=new URLSearchParams(location.search).get('c')||'';
function target(f){let d=$('d').value.trim().replace(/\/+$/,'');if(d[0]!='/')d='/'+d;
let n=f.webkitRelativePath||f.name;
if(!f.webkitRelativePath&&d=='/maps'&&/\.pmtiles$/i.test(n))n=n.replace(/\.pmtiles$/i,'')+'/'+n;
return(d=='/'?'':d)+'/'+n}
function put(f,path,done,all){return new Promise((ok,no)=>{const x=new XMLHttpRequest();
x.open('PUT','/upload?code='+encodeURIComponent($('c').value)+'&path='+encodeURIComponent(path));
x.upload.onprogress=e=>{$('p').value=(done+e.loaded)/all};
x.onload=()=>x.status==200?ok():no(x.responseText||x.status);x.onerror=()=>no('connection lost');x.send(f)})}
$('b').onclick=async()=>{const fs=[...$('f').files,...$('g').files];if(!fs.length){L.textContent='Choose files first';return}
const r=await fetch('/check?code='+encodeURIComponent($('c').value)).catch(()=>null);
if(!r||!r.ok){L.textContent=r?'Wrong code':'The radio does not answer';return}
const all=fs.reduce((s,f)=>s+f.size,0)||1;let done=0,n=0;$('b').disabled=true;L.textContent='';
for(const f of fs){const p=target(f);L.textContent+=p+' ... ';
try{await put(f,p,done,all);L.textContent+='saved\n';n++}catch(e){L.textContent+='FAILED: '+e+'\n'}
done+=f.size;$('p').value=done/all}
L.textContent+=n+' of '+fs.length+' files saved';$('b').disabled=false}
</script>
)HTML";

/**
 * Bytes waiting on the connection: more than 0 read, 0 nothing there yet, less the other side is gone.
 */
int receive(WiFiClient &client, uint8_t *buf, size_t size)
{
#if defined(ARCH_PORTDUINO)
    errno = 0;
    const int got = client.read(buf, size);
    if (got > 0)
        return got;
    if (got == 0)
        return -1;
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
#else
    const int waiting = client.available();
    if (waiting <= 0)
        return client.connected() ? 0 : -1;
    const int got = client.read(buf, std::min<size_t>(size, (size_t)waiting));
    return got > 0 ? got : 0;
#endif
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

std::string urlDecoded(const std::string &text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '%' && i + 2 < text.size() && hexValue(text[i + 1]) >= 0 && hexValue(text[i + 2]) >= 0) {
            out += (char)(hexValue(text[i + 1]) * 16 + hexValue(text[i + 2]));
            i += 2;
        } else {
            out += text[i] == '+' ? ' ' : text[i];
        }
    }
    return out;
}

// value of a parameter of a query string, decoded
std::string parameter(const std::string &query, const char *name)
{
    const std::string key = std::string(name) + "=";
    for (size_t at = 0; at < query.size();) {
        size_t end = query.find('&', at);
        if (end == std::string::npos)
            end = query.size();
        if (query.compare(at, key.size(), key) == 0)
            return urlDecoded(query.substr(at + key.size(), end - at - key.size()));
        at = end + 1;
    }
    return "";
}

/**
 * Makes a path of the storage out of what the browser asked for; empty when it will not do.
 */
std::string cleanPath(const std::string &asked)
{
    std::string out;
    size_t at = 0;
    while (at < asked.size()) {
        size_t end = asked.find('/', at);
        if (end == std::string::npos)
            end = asked.size();
        const std::string part = asked.substr(at, end - at);
        at = end + 1;
        if (part.empty() || part == ".")
            continue;
        if (part == "..")
            return "";
        for (unsigned char c : part) {
            if (c < 0x20 || c == 0x7F || strchr("\\:*?\"<>|", c))
                return "";
        }
        out += "/" + part;
    }
    if (asked.empty() || asked.back() == '/' || out.size() >= c_pathMax)
        return "";
    return out;
}

// value of a header line, lower case name with the colon
std::string headerValue(const std::string &header, const char *name)
{
    std::string lower = header;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)tolower(c); });
    size_t at = lower.find(std::string("\r\n") + name);
    if (at == std::string::npos)
        return "";
    at += 2 + strlen(name);
    size_t end = header.find("\r\n", at);
    if (end == std::string::npos)
        end = header.size();
    while (at < end && header[at] == ' ')
        at++;
    return header.substr(at, end - at);
}

} // namespace

struct UploadServer::Impl {
    enum class State { Idle, Header, Body, Drain };

    explicit Impl(IStorage &storage) : storage(storage), server(c_port) {}

    IStorage &storage;
    WiFiServer server;
    WiFiClient client;
    State state = State::Idle;
    std::string header;
    std::unique_ptr<IFile> out;
    std::string path;    // of the file arriving or arrived last
    uint64_t length = 0; // of the body
    uint64_t got = 0;
    uint64_t drain = 0; // bytes of a refused body still to throw away
    uint32_t lastData = 0;
    uint32_t began = 0; // when the state that has a limit on its whole length began
    uint32_t saved = 0;
    uint32_t failed = 0;
    uint32_t wrongCodes = 0;
    char code[8] = "";
    uint8_t chunk[c_chunk];

    void reply(int status, const char *text, const char *type = "text/plain; charset=utf-8", size_t size = 0)
    {
        static const struct {
            int status;
            const char *name;
        } names[] = {{200, "OK"},          {400, "Bad Request"},     {403, "Forbidden"},      {404, "Not Found"},
                     {405, "Not Allowed"}, {411, "Length Required"}, {431, "Header Too Big"}, {500, "Server Error"},
                     {507, "No Space"}};
        const char *name = "Error";
        for (const auto &known : names) {
            if (known.status == status)
                name = known.name;
        }
        if (!size)
            size = strlen(text);
        char head[160];
        const int len = snprintf(head, sizeof(head),
                                 "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
                                 "Connection: close\r\n\r\n",
                                 status, name, type, (unsigned)size);
        client.write((const uint8_t *)head, (size_t)len);
        // in pieces: a slow link takes only so much at once
        for (size_t at = 0; at < size;) {
            const size_t part = std::min<size_t>(size - at, 1024);
            const size_t sent = client.write((const uint8_t *)text + at, part);
            if (!sent || sent > part)
                break;
            at += sent;
        }
    }

    void close(void)
    {
        client.stop();
        header.clear();
        state = State::Idle;
    }

    // the upload broke off: nothing half written stays on the card
    void abandon(const char *why)
    {
        if (out) {
            out.reset();
            storage.remove(path.c_str());
            failed++;
            ILOG_WARN("upload of %s abandoned: %s", path.c_str(), why);
        }
        close();
    }

    bool codeFits(const std::string &given)
    {
        if (wrongCodes >= c_wrongCodes)
            return false;
        if (given == code)
            return true;
        wrongCodes++;
        return false;
    }

    // refuse an upload; what the browser is still sending gets thrown away so that it reads the answer
    void refuse(int status, const char *text, uint64_t waiting)
    {
        failed++;
        reply(status, text);
        if (!waiting) {
            close();
            return;
        }
        // only so much of it, and only so long: a client that was refused must not keep the one connection
        drain = std::min(waiting, c_drainMax);
        state = State::Drain;
        lastData = began = millis();
    }

    /**
     * The request is complete up to its body; rest is what arrived of the body already.
     */
    void request(const std::string &rest)
    {
        const size_t lineEnd = header.find("\r\n");
        const std::string line = header.substr(0, lineEnd);
        const size_t space = line.find(' '), space2 = line.rfind(' ');
        if (space == std::string::npos || space2 <= space) {
            reply(400, "Not a request");
            close();
            return;
        }
        const std::string method = line.substr(0, space);
        std::string target = line.substr(space + 1, space2 - space - 1), query;
        const size_t mark = target.find('?');
        if (mark != std::string::npos) {
            query = target.substr(mark + 1);
            target.resize(mark);
        }

        if (method == "GET" && (target == "/" || target == "/index.html")) {
            reply(200, c_page, "text/html; charset=utf-8", sizeof(c_page) - 1);
            close();
        } else if (method == "GET" && target == "/check") {
            if (codeFits(parameter(query, "code")))
                reply(200, "ok");
            else
                reply(403, wrongCodes >= c_wrongCodes ? "Too many wrong codes, start receiving again on the radio" : "Wrong code");
            close();
        } else if ((method == "PUT" || method == "POST") && target == "/upload") {
            const std::string lengthText = headerValue(header, "content-length:");
            const uint64_t waiting = lengthText.empty() ? 0 : strtoull(lengthText.c_str(), nullptr, 10);
            const uint64_t left = waiting > rest.size() ? waiting - rest.size() : 0;
            if (lengthText.empty()) {
                refuse(411, "The upload has no length", 0);
                return;
            }
            if (!codeFits(parameter(query, "code"))) {
                refuse(403, "Wrong code", left);
                return;
            }
            const std::string wanted = cleanPath(parameter(query, "path"));
            if (wanted.empty()) {
                refuse(400, "That path is not allowed", left);
                return;
            }
            const size_t slash = wanted.rfind('/');
            if (slash > 0)
                storage.mkdir(wanted.substr(0, slash).c_str());
            out = storage.open(wanted.c_str(), OpenMode::Write);
            if (!out) {
                refuse(500, "Cannot write there, is the card in?", left);
                return;
            }
            path = wanted;
            length = waiting;
            got = 0;
            state = State::Body;
            lastData = millis();
            ILOG_INFO("upload of %s, %u bytes", path.c_str(), (unsigned)length);
            if (headerValue(header, "expect:").find("100") != std::string::npos)
                client.write((const uint8_t *)"HTTP/1.1 100 Continue\r\n\r\n", 25);
            if (!rest.empty())
                body((const uint8_t *)rest.data(), rest.size());
            else if (!length)
                body(nullptr, 0);
        } else {
            reply(method == "GET" ? 404 : 405, "Nothing here");
            close();
        }
    }

    void body(const uint8_t *data, size_t size)
    {
        if (size > length - got)
            size = (size_t)(length - got);
        if (size && out->write(data, (uint32_t)size) != (int32_t)size) {
            const uint64_t left = length - got - size;
            out.reset();
            storage.remove(path.c_str());
            ILOG_WARN("upload of %s: the card does not take it", path.c_str());
            refuse(507, "Writing to the card failed (full, or a card error)", left);
            return;
        }
        got += size;
        if (got >= length) {
            out.reset();
            saved++;
            ILOG_INFO("upload of %s complete", path.c_str());
            reply(200, "saved");
            close();
        }
    }

    // one step; false when there is nothing to do right now
    bool step(void)
    {
        switch (state) {
        case State::Idle: {
#if defined(ARCH_PORTDUINO)
            client = server.available();
#else
            client = server.accept();
#endif
            if (!client)
                return false;
            header.clear();
            state = State::Header;
            lastData = began = millis();
            return true;
        }
        case State::Header: {
            const int size = receive(client, chunk, 512);
            if (size < 0 || (size == 0 && millis() - lastData > c_headerTimeout) || millis() - began > c_headerTotal) {
                close();
                return size < 0;
            }
            if (size == 0)
                return false;
            lastData = millis();
            header.append((const char *)chunk, (size_t)size);
            const size_t end = header.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (header.size() > c_headerMax) {
                    reply(431, "Request too long");
                    close();
                }
                return true;
            }
            const std::string rest = header.substr(end + 4);
            header.resize(end + 2);
            request(rest);
            return true;
        }
        case State::Body: {
            const int size = receive(client, chunk, sizeof(chunk));
            if (size < 0 || (size == 0 && millis() - lastData > c_bodyTimeout)) {
                abandon(size < 0 ? "connection lost" : "no data");
                return size < 0;
            }
            if (size == 0)
                return false;
            lastData = millis();
            body(chunk, (size_t)size);
            return true;
        }
        case State::Drain: {
            const int size = receive(client, chunk, sizeof(chunk));
            if (size < 0 || (size == 0 && millis() - lastData > c_drainTimeout) || millis() - began > c_drainTotal) {
                close();
                return size < 0;
            }
            if (size == 0)
                return false;
            lastData = millis();
            drain -= std::min<uint64_t>(drain, (uint64_t)size);
            if (!drain)
                close();
            return true;
        }
        }
        return false;
    }
};

UploadServer::UploadServer(IStorage &storage) : impl(new Impl(storage))
{
#if defined(ARCH_PORTDUINO)
    const uint32_t number = std::random_device{}();
#else
    const uint32_t number = esp_random();
#endif
    snprintf(impl->code, sizeof(impl->code), "%06u", (unsigned)(number % 1000000));
    impl->server.begin();
    ILOG_INFO("upload page on port %u", (unsigned)c_port);
}

UploadServer::~UploadServer()
{
    impl->abandon("stopped");
    delete impl;
    ILOG_INFO("upload page closed");
}

void UploadServer::poll(uint32_t budget)
{
    const uint32_t start = millis();
    while (impl->step() && millis() - start < budget) {
    }
}

const char *UploadServer::code(void) const
{
    return impl->code;
}

bool UploadServer::busy(void) const
{
    return impl->state == Impl::State::Body;
}

const std::string &UploadServer::file(void) const
{
    return impl->path;
}

uint64_t UploadServer::received(void) const
{
    return impl->got;
}

uint64_t UploadServer::expected(void) const
{
    return impl->length;
}

uint32_t UploadServer::saved(void) const
{
    return impl->saved;
}

uint32_t UploadServer::failed(void) const
{
    return impl->failed;
}

#endif
