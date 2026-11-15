// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vk12_internal.h
/// \brief vk12's own classes, shared by its source files

#ifndef RR_VK12_INTERNAL_H
#define RR_VK12_INTERNAL_H

#include "vk12.h"
#include "vk12_bases.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace rr_vk12
{

void Log(const char *format, ...);
#define VK12_LOG_ONCE(...) do { static bool logged_ = false; if (!logged_) { logged_ = true; rr_vk12::Log(__VA_ARGS__); } } while (0)

// HRESULT of a VkResult; notes a lost device
HRESULT Result(VkResult result, const char *what);

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

	// ID3D12Object's, for the interfaces that have it (not marked override: a blob isn't one)
	virtual HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name)
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

void SetDebugName(VkObjectType type, uint64_t handle, const std::string &name);

struct FormatInfo
{
	VkFormat format;
	unsigned bytes; // per texel
	VkImageAspectFlags aspect;
};
FormatInfo Format(DXGI_FORMAT format);

class Resource;
class Pipeline;

// What a descriptor heap slot holds. RTV, DSV and SRV slots refer to a view the resource owns, so rewriting a slot
// frees nothing: the driver rewrites slots while recorded commands still use the old views.
struct Slot
{
	Resource *resource = nullptr; // not owned: Direct3D 12 doesn't keep a resource alive for its descriptors either
	VkImageView view = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	uint32_t width = 0, height = 0;
	uint64_t serial = 0; // changes whenever the slot is written
};

class DescriptorHeap : public Object<ID3D12DescriptorHeapBase, ID3D12DescriptorHeap>
{
public:
	explicit DescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC &desc);
	~DescriptorHeap() override;
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

class Heap : public Object<ID3D12HeapBase, ID3D12Heap>
{
public:
	Heap(const D3D12_HEAP_DESC &desc, VkDeviceMemory memory, uint32_t type) : desc_(desc), memory_(memory), type_(type) {}
	~Heap() override;
	D3D12_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
	VkDeviceMemory Memory() const { return memory_; }
	uint32_t MemoryType() const { return type_; }

private:
	D3D12_HEAP_DESC desc_;
	VkDeviceMemory memory_;
	uint32_t type_;
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

	// The view of one kind the resource keeps, made on first use
	VkImageView View(VkFormat format, VkImageViewType type, VkImageAspectFlags aspect, uint32_t mips);

	D3D12_RESOURCE_DESC desc = {};
	VkBuffer buffer = VK_NULL_HANDLE;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE; // owned (committed), or null when placed in a heap or a swap chain's
	Heap *heap = nullptr; // held while placed in it
	void *mapped = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS address = 0;
	VkDeviceSize bytes = 0; // a buffer's size
	FormatInfo format = {};
	bool swapchain = false;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; // as of the commands recorded so far
	D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
	bool pendingInitialLayout = false;
	bool zeroFill = false; // cleared to zero with its first layout: a null view's image

protected:
	void Named() override;

private:
	struct KeptView
	{
		VkFormat format;
		VkImageViewType type;
		VkImageAspectFlags aspect;
		uint32_t mips;
		VkImageView view;
	};
	std::vector<KeptView> views_;
};

VkImageLayout LayoutOf(const Resource *resource, D3D12_RESOURCE_STATES state);

class RootSignature : public Object<ID3D12RootSignatureBase, ID3D12RootSignature>
{
};

class QueryHeap : public Object<ID3D12QueryHeapBase, ID3D12QueryHeap>
{
public:
	QueryHeap(VkQueryPool pool, uint32_t count) : pool(pool), count(count) {}
	~QueryHeap() override;
	VkQueryPool pool;
	uint32_t count;
};

class Blob : public Object<ID3D10BlobBase, ID3D10Blob>
{
public:
	LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return data.data(); }
	SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return data.size(); }
	std::vector<unsigned char> data;
};

// Root parameters of RR_ROOT_SIGNATURE, in its order, and where each lands in the one descriptor set
enum
{
	kCbvCount = 6, // b0..b5 at bindings 0..5
	kSrvCount = 11, // t0..t10 at bindings 16..26
	kSamplerCount = 2, // s0..s1 at bindings 32..33
	kSrvBinding = 16,
	kSamplerBinding = 32,
	kRootParameters = 19,
};
struct RootParameter
{
	enum Kind { CBV, SRV, SAMPLER } kind;
	unsigned index; // b, t or s register
};
extern const RootParameter kRoot[kRootParameters];

// A patched shader stage: the SPIR-V with every user input and output at the location its HLSL semantic names, and
// what the module says of itself
struct ShaderInfo
{
	std::vector<uint32_t> words;
	std::string entry;
	struct Input { std::string semantic; uint32_t location; };
	std::vector<Input> inputs; // a vertex shader's
	uint32_t uniformBytes[kCbvCount] = {}; // sizes of the uniform blocks at bindings 0..5; 0 where the stage has none
	bool usesBinding[40] = {}; // which bindings of the set the stage declares
};
bool PatchShader(const void *code, size_t bytes, ShaderInfo &info, std::string &error);
// The location of a semantic ("TEXCOORD", 1), the same in every stage; UINT32_MAX if vk12 has none for it
uint32_t SemanticLocation(const char *name, unsigned index);

struct PassKey
{
	VkFormat color, depth;
	bool operator<(const PassKey &other) const { return color != other.color ? color < other.color : depth < other.depth; }
};

class Pipeline : public Object<ID3D12PipelineStateBase, ID3D12PipelineState>
{
public:
	~Pipeline() override;
	// The pipeline for a topology and the formats of the targets being drawn to; made on first use if it isn't one of
	// those made with the object
	VkPipeline Get(VkPrimitiveTopology topology, const PassKey &pass);
	HRESULT Init(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc);

	uint32_t uniformBytes[kCbvCount] = {};
	bool usesBinding[40] = {};
	uint32_t stride = 0;

private:
	VkPipeline Make(VkPrimitiveTopology topology, const PassKey &pass);
	struct Variant
	{
		VkPrimitiveTopology topology;
		PassKey pass;
		VkPipeline pipeline;
	};
	std::vector<Variant> variants_;
	VkShaderModule vs_ = VK_NULL_HANDLE, ps_ = VK_NULL_HANDLE;
	std::string vsEntry_, psEntry_;
	std::vector<VkVertexInputAttributeDescription> attributes_;
	D3D12_BLEND_DESC blend_ = {};
	D3D12_RASTERIZER_DESC raster_ = {};
	D3D12_DEPTH_STENCIL_DESC depth_ = {};
	D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType_ = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	PassKey declared_ = {};
};

class Fence;

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
	~Fence() override;
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

class Allocator : public Object<ID3D12CommandAllocatorBase, ID3D12CommandAllocator>
{
public:
	Allocator() = default;
	~Allocator() override;
	HRESULT Init();
	HRESULT STDMETHODCALLTYPE Reset() override;

	VkCommandBuffer TakeBuffer();
	VkDescriptorSet TakeSet();

private:
	VkCommandPool pool_ = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> buffers_;
	size_t usedBuffers_ = 0;
	std::vector<VkDescriptorPool> setPools_;
	size_t currentSetPool_ = 0;
};

class List : public Object<ID3D12GraphicsCommandListBase, ID3D12GraphicsCommandList>
{
public:
	HRESULT Init(Allocator *allocator);

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
	void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) override;
	void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start, UINT count,
		ID3D12Resource *dst, UINT64 offset) override;

	VkCommandBuffer Buffer() const { return cmd_; }
	bool Open() const { return open_; }

protected:
	bool Also(REFIID riid) override { return SameGuid(riid, __uuidof(ID3D12CommandList)); }

private:
	void ResetState();
	void EndPass();
	bool BeginPass();
	void FlushInitialLayouts();
	bool PrepareDraw();
	void ClearAttachment(const Slot &target, bool color, VkImageAspectFlags aspect, const VkClearValue &value,
		UINT rects, const D3D12_RECT *rect);

	Allocator *allocator_ = nullptr; // not held: the driver keeps it until the list's work is done
	VkCommandBuffer cmd_ = VK_NULL_HANDLE;
	bool open_ = false;

	Slot color_, depth_; // copies of the bound target views
	bool hasColor_ = false, hasDepth_ = false;
	bool inPass_ = false;
	PassKey passKey_ = {};
	VkExtent2D passExtent_ = {};

	Pipeline *pipeline_ = nullptr;
	VkPipeline boundPipeline_ = VK_NULL_HANDLE;
	VkPrimitiveTopology topology_ = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkViewport viewport_ = {};
	VkRect2D scissor_ = {};
	uint32_t stencilRef_ = 0;
	bool viewportDirty_ = true, scissorDirty_ = true, stencilDirty_ = true;

	struct BufferBinding
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceSize offset = 0;
	};
	BufferBinding cbv_[kCbvCount];
	Slot srv_[kSrvCount];
	Slot sampler_[kSamplerCount];
	bool setDirty_ = true; // a buffer, view or sampler of the set changed (offsets alone don't dirty it)
	VkDescriptorSet set_ = VK_NULL_HANDLE;
	uint32_t setRanges_[kCbvCount] = {};
	bool setWritten_[40] = {};

	BufferBinding vertex_;
	uint32_t vertexStride_ = 0;
	bool vertexDirty_ = true;
	BufferBinding index_;
	VkIndexType indexType_ = VK_INDEX_TYPE_UINT16;
	bool indexDirty_ = true;
};

// The device and everything global to it
struct Context
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	bool debugUtils = false;
	bool memoryBudget = false;
	bool anisotropy = false;
	bool lost = false;
	VkPhysicalDeviceProperties properties = {};
	VkPhysicalDeviceMemoryProperties memory = {};
	VkFormat depthStencil = VK_FORMAT_UNDEFINED;
	char description[512] = {};

	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	VkPipelineCache pipelineCache = VK_NULL_HANDLE;
	std::map<PassKey, VkRenderPass> passes;
	struct FramebufferKey
	{
		VkRenderPass pass;
		VkImageView color, depth;
		bool operator<(const FramebufferKey &other) const
		{
			if (pass != other.pass) return pass < other.pass;
			if (color != other.color) return color < other.color;
			return depth < other.depth;
		}
	};
	std::map<FramebufferKey, VkFramebuffer> framebuffers;
	std::vector<Resource *> initialLayouts; // images made since the last flush, in their order

	// Buffers by the made-up address ranges root CBVs and buffer views name them with
	std::map<D3D12_GPU_VIRTUAL_ADDRESS, Resource *> buffers;
	D3D12_GPU_VIRTUAL_ADDRESS nextAddress = 0x100000000ull;

	// Submissions, oldest first; each completes with its fence
	struct Submission
	{
		uint64_t id;
		VkFence fence;
	};
	std::deque<Submission> submissions;
	std::vector<VkFence> freeFences;
	uint64_t lastSubmission = 0, completedSubmission = 0;
	VkSemaphore nextWait = VK_NULL_HANDLE, nextSignal = VK_NULL_HANDLE;

	std::vector<Resource *> nullImages; // what null SRVs show, one per format and dimension asked for
	uint64_t nextSerial = 1;
	int validationMessages = 0;
};
extern Context g;

VkRenderPass RenderPass(const PassKey &key);
VkFramebuffer Framebuffer(VkRenderPass pass, const Slot *color, const Slot *depth, VkExtent2D extent);
void ForgetFramebuffers(VkImageView view);
bool FindBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, VkBuffer *buffer, VkDeviceSize *offset);
// Polls the queue's fences; with a submission and a time, waits for that one
void UpdateSubmissions(uint64_t waitFor = 0, DWORD ms = 0);
HRESULT CreateResource(const D3D12_HEAP_PROPERTIES *heapProperties, Heap *heap, UINT64 heapOffset,
	const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES state, Resource **out);
bool ImageInfo(const D3D12_RESOURCE_DESC &desc, VkImageCreateInfo &info, FormatInfo &format);
uint32_t MemoryType(uint32_t allowed, VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags fallback);
VkMemoryPropertyFlags HeapMemoryFlags(D3D12_HEAP_TYPE type, VkMemoryPropertyFlags *fallback);
Resource *NullImage(DXGI_FORMAT format, bool volume);

} // namespace rr_vk12

#endif
