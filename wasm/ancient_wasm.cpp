/* Copyright (C) Teemu Suutari */
/* WebAssembly entry points for ancient: scan a blob for compressed streams,
   identify a stream and decompress it. Results are handed back as JSON text
   or through a static output buffer, so the JavaScript side needs no embind. */

#include <ancient/ancient.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include <emscripten/emscripten.h>

#include "StoneCrackerDecompressor.hpp"


namespace
{

std::string jsonResult;
std::vector<uint8_t> outBuffer;
std::string lastName;

void appendEscaped(std::string &out,const std::string &s)
{
	for (char c:s)
	{
		if (c=='"' || c=='\\') { out+='\\'; out+=c; }
		else if (static_cast<unsigned char>(c)<0x20) { char tmp[8]; snprintf(tmp,sizeof(tmp),"\\u%04x",c); out+=tmp; }
		else out+=c;
	}
}

// Formats whose detection is too weak for scanning inside arbitrary data
// (Unix compressors and raw deflate). They give short false positives, and
// since most store no packed size they are slow to reject.
bool isWeakForScan(const std::string &name)
{
	static const char *prefixes[]={"C: ","F: ","z: ","Z: ","SCO","Vice:","Vic2:","raw: ","zlib: "};
	for (auto p:prefixes)
		if (name.rfind(p,0)==0) return true;
	return false;
}

// Headerless ByteKiller is verified by an XOR checksum over the whole packed
// stream, which the library recomputes at every candidate offset (O(n) each).
// A prefix-XOR table answers the same question in O(1), so offsets that only
// headerless ByteKiller could claim, and whose checksum fails, are skipped.
struct XorIndex
{
	const uint8_t *data=nullptr;
	size_t size=0;
	std::vector<uint32_t> x; // x[j] = word(j) ^ word(j-4) ^ word(j-8) ...

	static uint32_t word(const uint8_t *d,size_t j) { return (uint32_t(d[j])<<24)|(uint32_t(d[j+1])<<16)|(uint32_t(d[j+2])<<8)|d[j+3]; }

	void build(const uint8_t *d,size_t n)
	{
		if (d==data && n==size) return;
		data=d; size=n;
		x.assign(n>=4?n-3:0,0);
		for (size_t j=0;j<x.size();j++) x[j]=word(d,j)^(j>=4?x[j-4]:0);
	}

	// XOR of the words at s, s+4, ..., e-4 (e-s a multiple of 4).
	uint32_t range(size_t s,size_t e) const
	{
		if (e<=s) return 0;
		return x[e-4]^(s>=4?x[s-4]:0);
	}

	// True when the library would only consider headerless ByteKiller at i and it would fail.
	bool skipHeaderlessByteKiller(size_t i) const
	{
		if (i+12>size) return false;
		uint32_t hdr=word(data,i);
		if (!hdr || (hdr&3U) || hdr>0x1000000U) return false; // not the headerless shape: let the library decide
		if ((hdr>>24)!=0) return false;                        // could be a FourCC of another format
		if (ancient::internal::StoneCrackerDecompressor::detectHeader(hdr,0)) return false;
		size_t end=i+size_t(hdr)+12U;
		if (end>size) return true;                             // library rejects this cheaply too
		return range(i+12,end)!=word(data,i+8);
	}
};

XorIndex xorIndex;

}

extern "C" {

EMSCRIPTEN_KEEPALIVE void *anc_malloc(size_t size) { return malloc(size?size:1); }
EMSCRIPTEN_KEEPALIVE void anc_free(void *p) { if (p==xorIndex.data) xorIndex=XorIndex{}; free(p); }

// Scan data[start..end) for compressed streams. A stream may extend past end
// (up to size). flags bit 0: skip weakly detected formats (see isWeakForScan).
// Returns a pointer to a JSON string:
//   {"next":<offset to continue from>,"hits":[{"offset":..,"packed":..,"raw":..,"name":".."},...]}
// The same two-pass check as the CLI 'scan' command keeps false positives low.
EMSCRIPTEN_KEEPALIVE const char *anc_scan(const uint8_t *data,size_t size,size_t start,size_t end,int flags)
{
	if (end>size) end=size;
	std::string hits;
	xorIndex.build(data,size);
	size_t i=start;
	while (i<end)
	{
		if (xorIndex.skipHeaderlessByteKiller(i) || !ancient::Decompressor::detect(data+i,size-i))
		{
			i++;
			continue;
		}
		try
		{
			ancient::Decompressor d1{data+i,size-i,false,true};
			if ((flags&1) && isWeakForScan(d1.getName()))
			{
				i++;
				continue;
			}
			if (!d1.getPackedSize()) d1.decompress(true);
			size_t packed=d1.getPackedSize().value();
			ancient::Decompressor d2{data+i,packed,true,true};
			auto raw=d2.decompress(true);
			size_t rawSize=d2.getRawSize()?d2.getRawSize().value():raw.size();
			if (!hits.empty()) hits+=',';
			hits+="{\"offset\":"+std::to_string(i)+",\"packed\":"+std::to_string(packed)+
				",\"raw\":"+std::to_string(rawSize)+",\"name\":\"";
			appendEscaped(hits,d2.getName());
			hits+="\"}";
			i+=packed?packed:1;
			continue;
		} catch (const ancient::Error&) {
		} catch (const std::bad_alloc&) {
		} catch (const std::exception&) {
		}
		i++;
	}
	jsonResult="{\"next\":"+std::to_string(i)+",\"hits\":["+hits+"]}";
	return jsonResult.c_str();
}

// Decompress one stream. Returns output size, or -1 on failure.
// On success the bytes are at anc_out_ptr() and the format name at anc_name().
EMSCRIPTEN_KEEPALIVE int anc_decompress(const uint8_t *data,size_t size,int exactSizeKnown)
{
	outBuffer.clear();
	lastName.clear();
	try
	{
		ancient::Decompressor d{data,size,exactSizeKnown!=0,true};
		lastName=d.getName();
		outBuffer=d.decompress(true);
		return static_cast<int>(outBuffer.size());
	} catch (...) {
		return -1;
	}
}

// Try data[0..len) as one complete stream with exactly known size (needed by
// formats that keep their header at the end, such as PowerPacker). Returns the
// raw size if it decompresses and verifies, otherwise -1. Used to search for the
// end of such streams inside a larger blob.
EMSCRIPTEN_KEEPALIVE int anc_try_exact(const uint8_t *data,size_t len)
{
	try
	{
		ancient::Decompressor d{data,len,true,true};
		auto raw=d.decompress(true);
		return static_cast<int>(raw.size());
	} catch (...) {
		return -1;
	}
}

// Identify a stream without decompressing. Returns JSON or "null".
EMSCRIPTEN_KEEPALIVE const char *anc_identify(const uint8_t *data,size_t size)
{
	try
	{
		ancient::Decompressor d{data,size,true,false};
		jsonResult="{\"name\":\"";
		appendEscaped(jsonResult,d.getName());
		jsonResult+="\",\"packed\":"+(d.getPackedSize()?std::to_string(d.getPackedSize().value()):std::string("null"))+
			",\"raw\":"+(d.getRawSize()?std::to_string(d.getRawSize().value()):std::string("null"))+"}";
	} catch (...) {
		jsonResult="null";
	}
	return jsonResult.c_str();
}

EMSCRIPTEN_KEEPALIVE const uint8_t *anc_out_ptr() { return outBuffer.data(); }
EMSCRIPTEN_KEEPALIVE const char *anc_name() { return lastName.c_str(); }
EMSCRIPTEN_KEEPALIVE void anc_out_release() { std::vector<uint8_t>().swap(outBuffer); }

}
