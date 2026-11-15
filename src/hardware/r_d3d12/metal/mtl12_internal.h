// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  mtl12_internal.h
/// \brief mtl12's own classes, shared by its source files (Objective-C++, compiled with ARC)
///
/// Metal's headers come first, and nothing of Apple's may be included after this header: Microsoft's declarations
/// define "interface" as a macro, which Objective-C's @interface doesn't survive.

#ifndef RR_MTL12_INTERNAL_H
#define RR_MTL12_INTERNAL_H

#import <Metal/Metal.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// The objects Metal's validation wraps a device in say which object they wrap
@protocol RRMtl12ToolsObject
- (id)baseObject;
@end

// Windows' BOOL is a 32-bit integer and Objective-C's a bool, and Objective-C's is already declared. Direct3D's
// declarations get theirs under another name here, which is the same type the driver's C++ files know as BOOL, so the
// interfaces are the same on both sides. From here on, BOOL means Windows' in mtl12's files.
#undef TRUE
#undef FALSE
#define BOOL RR_MTL12_WINBOOL
// Objective-C's Nil is a macro, and Microsoft's WRL has a class of that name
#pragma push_macro("Nil")
#undef Nil

#include "mtl12.h"
#include "mtl12_bases.h"
#include "mtl12_msl.h"
#include "../spirv_shader.h"

#pragma pop_macro("Nil")

namespace rr_mtl12
{

void Log(const char *format, ...) __attribute__((format(printf, 1, 2)));
#define MTL12_LOG_ONCE(...) do { static bool logged_ = false; if (!logged_) { logged_ = true; rr_mtl12::Log(__VA_ARGS__); } } while (0)

inline bool SameGuid(REFIID a, REFIID b)
{
	return !memcmp(&a, &b, sizeof(GUID));
}

// Reference counting and QueryInterface for an object that is one interface (and the interfaces that one inherits)
template <typename Base, typename Interface>
class Object : public Base
{
public:
	virtual ~Object() = default;

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
	{
		if (!out)
			return E_POINTER;
		if (SameGuid(riid, __uuidof(IUnknown)) || SameGuid(riid, __uuidof(Interface)) || Also(riid))
		{
			*out = static_cast<Interface *>(this);
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return ++refs_;
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG left = --refs_;
		if (!left)
			delete this;
		return left;
	}

	// ID3D12Object's
	HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override
	{
		name_.clear();
		for (; name && *name; name++)
			name_ += *name < 128 ? (char)*name : '?';
		Named();
		return S_OK;
	}

protected:
	virtual bool Also(REFIID) { return false; } // the inherited interfaces
	virtual void Named() {}
	std::string name_;

private:
	std::atomic<ULONG> refs_{1};
};

// Returns object as the interface riid, consuming the reference the caller holds
template <typename T>
HRESULT Finish(T *object, REFIID riid, void **out)
{
	const HRESULT hr = object->QueryInterface(riid, out);
	object->Release();
	return hr;
}

struct FormatInfo
{
	MTLPixelFormat format;
	unsigned bytes; // per texel
	bool depthStencil;
};
FormatInfo Format(DXGI_FORMAT format);

class Resource;
class Pipeline;

// What a descriptor heap slot holds. RTV, DSV and SRV slots refer to a texture the resource owns, and sampler slots to
// one the device owns, so rewriting a slot frees nothing: the driver rewrites slots while recorded commands still use
// what they held. Nothing here is retained.
struct Slot
{
	Resource *resource = nullptr; // not owned: Direct3D 12 doesn't keep a resource alive for its descriptors either
	__unsafe_unretained id<MTLTexture> texture = nil;
	__unsafe_unretained id<MTLSamplerState> sampler = nil;
	MTLPixelFormat format = MTLPixelFormatInvalid;
	uint32_t width = 0, height = 0;
};

class DescriptorHeap : public Object<ID3D12DescriptorHeapBase, ID3D12DescriptorHeap>
{
public:
	explicit DescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC &desc) : desc_(desc), slots_(desc.NumDescriptors) {}
	D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
	D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override;
	D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override;

private:
	D3D12_DESCRIPTOR_HEAP_DESC desc_;
	std::vector<Slot> slots_;
};

// Handles are the slots' addresses
inline Slot *SlotOf(D3D12_CPU_DESCRIPTOR_HANDLE handle) { return reinterpret_cast<Slot *>(handle.ptr); }
inline Slot *SlotOf(D3D12_GPU_DESCRIPTOR_HANDLE handle) { return reinterpret_cast<Slot *>((uintptr_t)handle.ptr); }

// A heap holds no memory here: every resource placed in one is a Metal texture or buffer of its own. The driver's
// offsets into it only have to be the consistent bookkeeping they are.
class Heap : public Object<ID3D12HeapBase, ID3D12Heap>
{
public:
	explicit Heap(const D3D12_HEAP_DESC &desc) : desc_(desc) {}
	D3D12_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }

private:
	D3D12_HEAP_DESC desc_;
};

class Resource : public Object<ID3D12ResourceBase, ID3D12Resource>
{
public:
	Resource() = default;
	~Resource() override;

	HRESULT STDMETHODCALLTYPE Map(UINT subresource, const D3D12_RANGE *read, void **data) override;
	void STDMETHODCALLTYPE Unmap(UINT, const D3D12_RANGE *) override {}
	D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return desc; }
	D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override { return address; }

	// The texture itself, or the view of some of its mips that the resource keeps, made on first use
	id<MTLTexture> View(MTLPixelFormat format, MTLTextureType type, uint32_t mips);

	D3D12_RESOURCE_DESC desc = {};
	id<MTLBuffer> buffer = nil;
	id<MTLTexture> texture = nil;
	Heap *heap = nullptr; // held while placed in it
	void *mapped = nullptr; // an upload or readback buffer's contents
	D3D12_GPU_VIRTUAL_ADDRESS address = 0;
	uint64_t bytes = 0; // a buffer's size
	FormatInfo format = {};

protected:
	void Named() override;

private:
	struct KeptView
	{
		MTLPixelFormat format;
		MTLTextureType type;
		uint32_t mips;
		id<MTLTexture> view;
	};
	std::vector<KeptView> views_;
};

class RootSignature : public Object<ID3D12RootSignatureBase, ID3D12RootSignature>
{
};

// Metal has no timestamps a command list can write into a buffer, so the heap is a count and its queries read as zero
class QueryHeap : public Object<ID3D12QueryHeapBase, ID3D12QueryHeap>
{
public:
	explicit QueryHeap(uint32_t count) : count(count) {}
	uint32_t count;
};

using rr_spirv::kCbvCount;
using rr_spirv::kSrvCount;
using rr_spirv::kSamplerCount;
using rr_spirv::kSrvBinding;
using rr_spirv::kSamplerBinding;
using rr_spirv::kRootParameters;
using rr_spirv::RootParameter;
using rr_spirv::kRoot;

// A stage compiled for Metal, and what its SPIR-V said of itself. Shared by every pipeline state made with the same
// shader, which is most of them: the driver makes hundreds of states from two dozen shaders. It lasts as long as one
// of those pipeline states does.
struct Shader
{
	id<MTLFunction> function = nil;
	std::vector<rr_spirv::ShaderInfo::Input> inputs; // a vertex shader's
	bool usesBinding[40] = {}; // the bindings the Metal function takes
};
std::shared_ptr<Shader> GetShader(const D3D12_SHADER_BYTECODE &code, bool vertex);
void ForgetShaders();
// How many shaders the cache knows
size_t ShaderCount();

// The formats of the targets a pipeline draws to; MTLPixelFormatInvalid for none
struct PassKey
{
	MTLPixelFormat color, depth;
};

class Pipeline : public Object<ID3D12PipelineStateBase, ID3D12PipelineState>
{
public:
	~Pipeline() override;
	HRESULT Init(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc);
	// The Metal pipeline state for the formats of the targets being drawn to; made on first use if it isn't the one
	// made with the object
	id<MTLRenderPipelineState> Get(const PassKey &pass);

	// What Direct3D 12 has in the pipeline state and Metal sets on the encoder
	id<MTLDepthStencilState> depthStencil = nil;
	MTLCullMode cull = MTLCullModeNone;
	MTLWinding winding = MTLWindingClockwise;
	float depthBias = 0.0f, depthBiasSlope = 0.0f, depthBiasClamp = 0.0f;
	MTLDepthClipMode depthClip = MTLDepthClipModeClip;

	// The bindings each stage's function takes: as SPIRV-Cross wrote them, and then as Metal says the compiled pipeline
	// really uses them, where it can say (macOS 13)
	bool vertexUses[40] = {}, pixelUses[40] = {};
	uint32_t stride = 0;
	bool readsVertices = false;

private:
	id<MTLRenderPipelineState> Make(const PassKey &pass);
	struct Variant
	{
		PassKey pass;
		id<MTLRenderPipelineState> state;
	};
	std::vector<Variant> variants_;
	bool reflected_ = false;
	std::shared_ptr<Shader> vs_, ps_;
	MTLVertexDescriptor *vertexDescriptor_ = nil;
	D3D12_BLEND_DESC blend_ = {};
	PassKey declared_ = {};
};

class Queue : public Object<ID3D12CommandQueueBase, ID3D12CommandQueue>
{
public:
	void STDMETHODCALLTYPE ExecuteCommandLists(UINT count, ID3D12CommandList *const *lists) override;
	HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence *fence, UINT64 value) override;
	HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64 *frequency) override;
	D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return {}; }
};

class Fence : public Object<ID3D12FenceBase, ID3D12Fence>
{
public:
	explicit Fence(UINT64 value) : completed_(value) {}
	UINT64 STDMETHODCALLTYPE GetCompletedValue() override;
	HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override;

	void Pend(UINT64 value, uint64_t submission);
	bool Wait(UINT64 value, DWORD ms);

private:
	struct Pending
	{
		UINT64 value;
		uint64_t submission; // completes when this submission of the queue does; 0: at once
	};
	std::deque<Pending> pending_;
	UINT64 completed_;
};

// Metal's command buffers come from the queue and free themselves, so an allocator is only the object the driver
// expects to reset
class Allocator : public Object<ID3D12CommandAllocatorBase, ID3D12CommandAllocator>
{
public:
	HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
};

class List : public Object<ID3D12GraphicsCommandListBase, ID3D12GraphicsCommandList>
{
public:
	~List() override;
	HRESULT STDMETHODCALLTYPE Close() override;
	HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator *allocator, ID3D12PipelineState *initial) override;
	D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return D3D12_COMMAND_LIST_TYPE_DIRECT; }

	void STDMETHODCALLTYPE DrawInstanced(UINT vertices, UINT instances, UINT firstVertex, UINT firstInstance) override;
	void STDMETHODCALLTYPE DrawIndexedInstanced(UINT indices, UINT instances, UINT firstIndex, INT baseVertex,
		UINT firstInstance) override;
	void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z,
		const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box) override;
	void STDMETHODCALLTYPE CopyResource(ID3D12Resource *dst, ID3D12Resource *src) override;
	void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource *dst, UINT64 dstOffset, ID3D12Resource *src, UINT64 srcOffset,
		UINT64 bytes) override;
	void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) override;
	void STDMETHODCALLTYPE RSSetViewports(UINT count, const D3D12_VIEWPORT *viewports) override;
	void STDMETHODCALLTYPE RSSetScissorRects(UINT count, const D3D12_RECT *rects) override;
	void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT factor[4]) override;
	void STDMETHODCALLTYPE OMSetStencilRef(UINT ref) override;
	void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState *pipeline) override;
	void STDMETHODCALLTYPE ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER *barriers) override;
	void STDMETHODCALLTYPE SetDescriptorHeaps(UINT, ID3D12DescriptorHeap *const *) override {}
	void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature *) override {}
	void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT parameter, D3D12_GPU_DESCRIPTOR_HANDLE base) override;
	void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT parameter, D3D12_GPU_VIRTUAL_ADDRESS address) override;
	void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *view) override;
	void STDMETHODCALLTYPE IASetVertexBuffers(UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW *views) override;
	void STDMETHODCALLTYPE OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *targets, BOOL single,
		const D3D12_CPU_DESCRIPTOR_HANDLE *depth) override;
	void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags, FLOAT depth,
		UINT8 stencil, UINT rects, const D3D12_RECT *rect) override;
	void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE view, const FLOAT color[4], UINT rects,
		const D3D12_RECT *rect) override;
	void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT) override {}
	void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT) override {}
	void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start, UINT count,
		ID3D12Resource *dst, UINT64 offset) override;

	bool Open() const { return open_; }
	// The closed list's command buffer, for the queue to commit; the list has none until it is reset
	id<MTLCommandBuffer> TakeBuffer();

protected:
	bool Also(REFIID riid) override { return SameGuid(riid, __uuidof(ID3D12CommandList)); }

private:
	struct BufferBinding
	{
		__unsafe_unretained id<MTLBuffer> buffer = nil;
		uint64_t offset = 0;
	};
	// A clear waiting for the pass that draws to its texture, where it is that attachment's load action
	struct PendingClear
	{
		__unsafe_unretained id<MTLTexture> texture = nil;
		bool color = false, depth = false, stencil = false;
		MTLClearColor colorValue = {};
		double depthValue = 1.0;
		uint32_t stencilValue = 0;
	};

	void ResetState();
	void ResetEncoderState();
	void EndPass();
	void EndBlit();
	bool BeginPass();
	id<MTLBlitCommandEncoder> Blit(id<MTLTexture> first, id<MTLTexture> second);
	bool PrepareDraw();
	MTLPrimitiveType Primitive() const;
	PendingClear *FindClear(id<MTLTexture> texture);
	PendingClear &AddClear(const Slot &target);
	void TakeClear(id<MTLTexture> texture, MTLRenderPassDescriptor *pass);
	void FlushClear(id<MTLTexture> texture);
	void FlushClears(id<MTLTexture> exceptFirst, id<MTLTexture> exceptSecond);
	void BindBuffer(bool vertexStage, unsigned index, const BufferBinding &binding);

	id<MTLCommandBuffer> cmd_ = nil;
	id<MTLRenderCommandEncoder> pass_ = nil;
	id<MTLBlitCommandEncoder> blit_ = nil;
	unsigned passDraws_ = 0, blitCopies_ = 0; // what the open encoders hold, for Context's limits
	bool open_ = false;

	Slot color_, depth_; // copies of the bound target views
	bool hasColor_ = false, hasDepth_ = false;
	PassKey passKey_ = {};
	uint32_t passWidth_ = 0, passHeight_ = 0;
	std::vector<PendingClear> clears_;

	Pipeline *pipeline_ = nullptr;
	D3D12_PRIMITIVE_TOPOLOGY topology_ = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	MTLViewport viewport_ = {};
	D3D12_RECT scissor_ = {};
	uint32_t stencilRef_ = 0;
	float blendFactor_[4] = {};

	BufferBinding cbv_[kCbvCount];
	Slot srv_[kSrvCount];
	Slot sampler_[kSamplerCount];
	BufferBinding vertex_;
	uint32_t vertexStride_ = 0;
	BufferBinding index_;
	MTLIndexType indexType_ = MTLIndexTypeUInt16;

	// What the open pass's encoder has been told, so that a draw only sets what differs
	struct EncoderState
	{
		__unsafe_unretained id<MTLRenderPipelineState> pipeline = nil;
		__unsafe_unretained id<MTLDepthStencilState> depthStencil = nil;
		bool rasterSet = false;
		MTLCullMode cull = MTLCullModeNone;
		MTLWinding winding = MTLWindingClockwise;
		float depthBias = 0.0f, depthBiasSlope = 0.0f, depthBiasClamp = 0.0f;
		MTLDepthClipMode depthClip = MTLDepthClipModeClip;
		bool viewportSet = false, scissorSet = false, stencilSet = false, blendSet = false;
		MTLViewport viewport = {};
		MTLScissorRect scissor = {};
		uint32_t stencilRef = 0;
		float blendFactor[4] = {};
		BufferBinding vertexBuffers[kCbvCount], pixelBuffers[kCbvCount], vertexData;
		__unsafe_unretained id<MTLTexture> textures[kSrvCount] = {};
		__unsafe_unretained id<MTLSamplerState> samplers[kSamplerCount] = {};
	};
	EncoderState encoder_;
};

// The device and everything global to it
struct Context
{
	id<MTLDevice> device = nil;
	id<MTLCommandQueue> queue = nil;
	id<MTLDepthStencilState> noDepthStencil = nil; // for passes without a depth target
	id<MTLDrawable> nextDrawable = nil; // PresentWithNextSubmit's
	bool validation = false;
	bool unified = false, tiled = false;
	// How many draws a pass, and how many copies a blit encoder, may hold before a list ends it and goes on in another;
	// 0 for no limit. Set for Apple's paravirtual GPU (see CreateDevice).
	unsigned encoderDraws = 0, encoderCopies = 0;
	int failNextSubmission = 0; // FailNextSubmissionForTest's
	char description[512] = {};

	// Buffers by the made-up address ranges root CBVs and buffer views name them with
	std::map<D3D12_GPU_VIRTUAL_ADDRESS, Resource *> buffers;
	D3D12_GPU_VIRTUAL_ADDRESS nextAddress = 0x100000000ull;

	std::vector<Resource *> nullImages; // what null SRVs show, one per format and dimension asked for
};
extern Context g;

// What the command buffers' completion handlers, which Metal runs on threads of its own, share with the rest
struct Completions
{
	std::mutex mutex;
	std::condition_variable changed;
	uint64_t last = 0; // the newest submission
	std::set<uint64_t> running; // submissions whose command buffers haven't completed
	std::vector<std::string> messages; // for the log, which only the driver's thread writes
	// S_OK until a command buffer fails, then what the device says of itself (GetDeviceRemovedReason): from then on
	// fences read as UINT64_MAX and waits fail, as when Direct3D 12 removes a device
	std::atomic<HRESULT> failure{S_OK};
	std::atomic<uint64_t> errors{0};
	bool Failed() const { return failure != S_OK; }

	// Every submission up to this one is done. Call with the mutex held.
	uint64_t Completed() const { return running.empty() ? last : *running.begin() - 1; }
};
extern Completions completions;

// Writes what the completion handlers had to say to the log
void FlushMessages();
// A command buffer of the queue's, reporting as much as the device's options ask for
id<MTLCommandBuffer> NewCommandBuffer();
// Blocks until every submission has completed, or ms pass
bool WaitIdle(DWORD ms);

bool FindBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, id<MTLBuffer> __unsafe_unretained *buffer, uint64_t *offset);
HRESULT CreateResource(D3D12_HEAP_TYPE type, Heap *heap, const D3D12_RESOURCE_DESC &desc, Resource **out);
// The bytes of a texture's mips, without any padding
uint64_t TextureBytes(const D3D12_RESOURCE_DESC &desc, const FormatInfo &format);
Resource *NullImage(DXGI_FORMAT format, bool volume);

} // namespace rr_mtl12

#endif
