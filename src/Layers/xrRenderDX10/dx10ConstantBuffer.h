#ifndef	dx10ConstantBuffer_included
#define	dx10ConstantBuffer_included
#pragma once

struct R_constant;
struct R_constant_load;

class dx10ConstantBuffer : public xr_resource_named
{
public:
	dx10ConstantBuffer(ID3DShaderReflectionConstantBuffer* pTable);
	~dx10ConstantBuffer();

	bool Similar(dx10ConstantBuffer& _in);
	ID3DBuffer* GetBuffer() { return m_pBuffer; }
	// The render (CBackend::constants_epoch) the per-frame constant handlers last wrote this buffer in. Buffers with
	// one layout are shared by every shader using it, so a table whose buffers were all written in this render skips
	// its per-frame handlers (CBackend::set_Constants)
	u32 frame_written = u32(-1);

	void Flush();

	//	Set copy data into constant buffer
	//	Plain buffer member
	void set(R_constant* C, R_constant_load& L, const Fmatrix& A);
	void set(R_constant* C, R_constant_load& L, const Fvector4& A);
	void set(R_constant* C, R_constant_load& L, float A);
	void set(R_constant* C, R_constant_load& L, int A);
	//	Array buffer member
	void seta(R_constant* C, R_constant_load& L, u32 e, const Fmatrix& A);
	void seta(R_constant* C, R_constant_load& L, u32 e, const Fvector4& A);
	//	Raw bytes over the declared size, the tail past the data is zeroed
	void set_bytes(R_constant_load& L, const void* data, u32 bytes);

	void* AccessDirect(R_constant_load& L, u32 DataSize);

private:
	Fvector4* Access(u16 offset);
	// Copies data at offset; the buffer is uploaded again only when the data differs
	void Write(u32 offset, const void* data, u32 size);

private:
	shared_str m_strBufferName;
	D3D_CBUFFER_TYPE m_eBufferType;

	//	Buffer data description
	u32 m_uiMembersCRC;
	xr_vector<D3D_SHADER_TYPE_DESC> m_MembersList;
	xr_vector<shared_str> m_MembersNames;

	ID3DBuffer* m_pBuffer;
	u32 m_uiBufferSize; //	Cache buffer size for debug validation
	void* m_pBufferData;
	bool m_bChanged;
	// Tree instancing's xr_tree_instances is only written with AccessDirect, a few trees of its 32 at a time: Flush
	// uploads the part the last AccessDirect wrote (the shader reads no further)
	bool m_bDirectOnly = false;
	u32 m_uiDirectEnd = 0;

	static const u32 lineSize = sizeof(Fvector4);

	//	Never try to copy objects of this class due to the pointer and autoptr members
	dx10ConstantBuffer(const dx10ConstantBuffer&);
	dx10ConstantBuffer& operator=(dx10ConstantBuffer&);
};

typedef resptr_core<dx10ConstantBuffer, resptr_base<dx10ConstantBuffer>> ref_cbuffer;

#endif	//	dx10ConstantBuffer_included
