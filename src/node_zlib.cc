// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.


#include <v8.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <zlib.h>

#include <node.h>
#include <node_buffer.h>
#include <req_wrap.h>



namespace node {
using namespace v8;

// write() returns one of these, and then calls the cb() when it's done.
typedef ReqWrap<uv_work_t> WorkReqWrap;
    
class ZlibStatics : public ModuleStatics {
public:
  Persistent<String> callback_sym;
};

enum node_zlib_mode {
  DEFLATE = 1,
  INFLATE,
  GZIP,
  GUNZIP,
  DEFLATERAW,
  INFLATERAW,
  UNZIP
};

template <node_zlib_mode mode> class ZCtx;


void InitZlib(v8::Handle<v8::Object> target);


/**
 * Deflate/Inflate
 */
template <node_zlib_mode mode> class ZCtx : public ObjectWrap {
 public:

  ZCtx() : ObjectWrap() {
  }

  ~ZCtx() {
    if (mode == DEFLATE || mode == GZIP || mode == DEFLATERAW) {
      (void)deflateEnd(&strm_);
    } else if (mode == INFLATE || mode == GUNZIP || mode == INFLATERAW) {
      (void)inflateEnd(&strm_);
    }
  }

  // write(flush, in, in_off, in_len, out, out_off, out_len)
  static Handle<Value>
  Write(const Arguments& args) {
    HandleScope scope;
    assert(args.Length() == 7);

    ZCtx<mode> *ctx = ObjectWrap::Unwrap< ZCtx<mode> >(args.This());
    assert(ctx->init_done_ && "write before init");

    unsigned int flush = args[0]->Uint32Value();
    Bytef *in;
    Bytef *out;
    size_t in_off, in_len, out_off, out_len;

    if (args[1]->IsNull()) {
      // just a flush
      Bytef nada[1] = { 0 };
      in = nada;
      in_len = 0;
      in_off = 0;
    } else {
      assert(Buffer::HasInstance(args[1]));
      Local<Object> in_buf;
      in_buf = args[1]->ToObject();
      in_off = (size_t)args[2]->Uint32Value();
      in_len = (size_t)args[3]->Uint32Value();

      assert(in_off + in_len <= Buffer::Length(in_buf));
      in = reinterpret_cast<Bytef *>(Buffer::Data(in_buf) + in_off);
    }

    assert(Buffer::HasInstance(args[4]));
    Local<Object> out_buf = args[4]->ToObject();
    out_off = (size_t)args[5]->Uint32Value();
    out_len = (size_t)args[6]->Uint32Value();
    assert(out_off + out_len <= Buffer::Length(out_buf));
    out = reinterpret_cast<Bytef *>(Buffer::Data(out_buf) + out_off);

    WorkReqWrap *req_wrap = new WorkReqWrap();

    req_wrap->data_ = ctx;
    ctx->strm_.avail_in = in_len;
    ctx->strm_.next_in = &(*in);
    ctx->strm_.avail_out = out_len;
    ctx->strm_.next_out = out;
    ctx->flush_ = flush;

    // set this so that later on, I can easily tell how much was written.
    ctx->chunk_size_ = out_len;

    // build up the work request
    uv_work_t* work_req = new uv_work_t();
    work_req->data = req_wrap;

    uv_queue_work(Isolate::GetCurrentLoop(),
                  work_req,
                  ZCtx<mode>::Process,
                  ZCtx<mode>::After);

    req_wrap->Dispatched();

    return req_wrap->object_;
  }


  // thread pool!
  // This function may be called multiple times on the uv_work pool
  // for a single write() call, until all of the input bytes have
  // been consumed.
  static void
  Process(uv_work_t* work_req) {
    WorkReqWrap *req_wrap = reinterpret_cast<WorkReqWrap *>(work_req->data);
    ZCtx<mode> *ctx = (ZCtx<mode> *)req_wrap->data_;

    // If the avail_out is left at 0, then it means that it ran out
    // of room.  If there was avail_out left over, then it means
    // that all of the input was consumed.
    int err;
    switch (mode) {
      case DEFLATE:
      case GZIP:
      case DEFLATERAW:
        err = deflate(&(ctx->strm_), ctx->flush_);
        break;
      case UNZIP:
      case INFLATE:
      case GUNZIP:
      case INFLATERAW:
        err = inflate(&(ctx->strm_), ctx->flush_);
        break;
      default:
        assert(0 && "wtf?");
    }
    assert(err != Z_STREAM_ERROR);

    // now After will emit the output, and
    // either schedule another call to Process,
    // or shift the queue and call Process.
  }

  // v8 land!
  static void
  After(uv_work_t* work_req) {
    HandleScope scope;
    ZlibStatics *statics = NODE_STATICS_GET(node_zlib, ZlibStatics);
    WorkReqWrap *req_wrap = reinterpret_cast<WorkReqWrap *>(work_req->data);
    ZCtx<mode> *ctx = (ZCtx<mode> *)req_wrap->data_;
    Local<Integer> avail_out = Integer::New(ctx->strm_.avail_out);
    Local<Integer> avail_in = Integer::New(ctx->strm_.avail_in);

    // call the write() cb
    assert(req_wrap->object_->Get(statics->callback_sym)->IsFunction() &&
           "Invalid callback");
    Local<Value> args[2] = { avail_in, avail_out };
    MakeCallback(req_wrap->object_, "callback", 2, args);

    // delete the ReqWrap
    delete req_wrap;
  }

  static Handle<Value>
  New(const Arguments& args) {
    HandleScope scope;
    ZCtx<mode> *ctx = new ZCtx<mode>();
    ctx->Wrap(args.This());
    return args.This();
  }

  // just pull the ints out of the args and call the other Init
  static Handle<Value>
  Init(const Arguments& args) {
    HandleScope scope;

    assert(args.Length() == 4 &&
           "init(windowBits, level, memLevel, strategy)");

    ZCtx<mode> *ctx = ObjectWrap::Unwrap< ZCtx<mode> >(args.This());

    int windowBits = args[0]->Uint32Value();
    assert((windowBits >= 8 && windowBits <= 15) && "invalid windowBits");

    int level = args[1]->Uint32Value();
    assert((level >= -1 && level <= 9) && "invalid compression level");

    int memLevel = args[2]->Uint32Value();
    assert((memLevel >= 1 && memLevel <= 9) && "invalid memlevel");

    int strategy = args[3]->Uint32Value();
    assert((strategy == Z_FILTERED ||
            strategy == Z_HUFFMAN_ONLY ||
            strategy == Z_RLE ||
            strategy == Z_FIXED ||
            strategy == Z_DEFAULT_STRATEGY) && "invalid strategy");

    Init(ctx, level, windowBits, memLevel, strategy);
    return Undefined();
  }

  static void
  Init(ZCtx *ctx,
       int level,
       int windowBits,
       int memLevel,
       int strategy) {
    ctx->level_ = level;
    ctx->windowBits_ = windowBits;
    ctx->memLevel_ = memLevel;
    ctx->strategy_ = strategy;

    ctx->strm_.zalloc = Z_NULL;
    ctx->strm_.zfree = Z_NULL;
    ctx->strm_.opaque = Z_NULL;

    ctx->flush_ = Z_NO_FLUSH;

    if (mode == GZIP || mode == GUNZIP) {
      ctx->windowBits_ += 16;
    }

    if (mode == UNZIP) {
      ctx->windowBits_ += 32;
    }

    if (mode == DEFLATERAW || mode == INFLATERAW) {
      ctx->windowBits_ *= -1;
    }

    int err;
    switch (mode) {
      case DEFLATE:
      case GZIP:
      case DEFLATERAW:
        err = deflateInit2(&(ctx->strm_),
                           ctx->level_,
                           Z_DEFLATED,
                           ctx->windowBits_,
                           ctx->memLevel_,
                           ctx->strategy_);
        break;
      case INFLATE:
      case GUNZIP:
      case INFLATERAW:
      case UNZIP:
        err = inflateInit2(&(ctx->strm_), ctx->windowBits_);
        break;
      default:
        assert(0 && "wtf?");
    }

    ctx->init_done_ = true;
    assert(err == Z_OK);
  }

 private:

  bool init_done_;

  z_stream strm_;
  int level_;
  int windowBits_;
  int memLevel_;
  int strategy_;

  int flush_;

  int chunk_size_;
};


#define NODE_ZLIB_CLASS(mode, name)   \
  { \
    Local<FunctionTemplate> z = FunctionTemplate::New(ZCtx<mode>::New); \
    z->InstanceTemplate()->SetInternalFieldCount(1); \
    NODE_SET_PROTOTYPE_METHOD(z, "write", ZCtx<mode>::Write); \
    NODE_SET_PROTOTYPE_METHOD(z, "init", ZCtx<mode>::Init); \
    z->SetClassName(String::NewSymbol(name)); \
    target->Set(String::NewSymbol(name), z->GetFunction()); \
  }

void InitZlib(Handle<Object> target) {
  HandleScope scope;
  NODE_STATICS_NEW(node_zlib, ZlibStatics, statics);

  NODE_ZLIB_CLASS(INFLATE, "Inflate")
  NODE_ZLIB_CLASS(DEFLATE, "Deflate")
  NODE_ZLIB_CLASS(INFLATERAW, "InflateRaw")
  NODE_ZLIB_CLASS(DEFLATERAW, "DeflateRaw")
  NODE_ZLIB_CLASS(GZIP, "Gzip")
  NODE_ZLIB_CLASS(GUNZIP, "Gunzip")
  NODE_ZLIB_CLASS(UNZIP, "Unzip")

  statics->callback_sym = NODE_PSYMBOL("callback");

  NODE_DEFINE_CONSTANT(target, Z_NO_FLUSH);
  NODE_DEFINE_CONSTANT(target, Z_PARTIAL_FLUSH);
  NODE_DEFINE_CONSTANT(target, Z_SYNC_FLUSH);
  NODE_DEFINE_CONSTANT(target, Z_FULL_FLUSH);
  NODE_DEFINE_CONSTANT(target, Z_FINISH);
  NODE_DEFINE_CONSTANT(target, Z_BLOCK);
  NODE_DEFINE_CONSTANT(target, Z_OK);
  NODE_DEFINE_CONSTANT(target, Z_STREAM_END);
  NODE_DEFINE_CONSTANT(target, Z_NEED_DICT);
  NODE_DEFINE_CONSTANT(target, Z_ERRNO);
  NODE_DEFINE_CONSTANT(target, Z_STREAM_ERROR);
  NODE_DEFINE_CONSTANT(target, Z_DATA_ERROR);
  NODE_DEFINE_CONSTANT(target, Z_MEM_ERROR);
  NODE_DEFINE_CONSTANT(target, Z_BUF_ERROR);
  NODE_DEFINE_CONSTANT(target, Z_VERSION_ERROR);
  NODE_DEFINE_CONSTANT(target, Z_NO_COMPRESSION);
  NODE_DEFINE_CONSTANT(target, Z_BEST_SPEED);
  NODE_DEFINE_CONSTANT(target, Z_BEST_COMPRESSION);
  NODE_DEFINE_CONSTANT(target, Z_DEFAULT_COMPRESSION);
  NODE_DEFINE_CONSTANT(target, Z_FILTERED);
  NODE_DEFINE_CONSTANT(target, Z_HUFFMAN_ONLY);
  NODE_DEFINE_CONSTANT(target, Z_RLE);
  NODE_DEFINE_CONSTANT(target, Z_FIXED);
  NODE_DEFINE_CONSTANT(target, Z_DEFAULT_STRATEGY);
  NODE_DEFINE_CONSTANT(target, ZLIB_VERNUM);

  target->Set(String::NewSymbol("ZLIB_VERSION"), String::New(ZLIB_VERSION));
}

}  // namespace node

NODE_MODULE(node_zlib, node::InitZlib)
