#ifndef	dx10ConstantBuffer_impl_included
#define	dx10ConstantBuffer_impl_included
#pragma once

IC Fvector4* dx10ConstantBuffer::Access(u16 offset)
{
	//	TODO: DX10: Implement code which will check if set actually changes code.
	m_bChanged = true;

	//	Check buffer size in client code: don't know if actual data will cross
	//	buffer boundaries.
	VERIFY(offset<(int)m_uiBufferSize);
	BYTE* res = ((BYTE*)m_pBufferData) + offset;
	return (Fvector4*)res;
}

extern int ps_r4_cbuffer_skip_unchanged;

IC void dx10ConstantBuffer::Write(u32 offset, const void* data, u32 size)
{
	VERIFY(offset + size <= m_uiBufferSize);
	BYTE* dst = ((BYTE*)m_pBufferData) + offset;
	if (!m_bChanged && ps_r4_cbuffer_skip_unchanged && !memcmp(dst, data, size))
		return;
	CopyMemory(dst, data, size);
	m_bChanged = true;
}

// Matrix as 2, 3 or 4 rows of the transposed matrix
IC u32 dx10ConstantBuffer_rows(u16 cls, const Fmatrix& A, Fvector4* rows)
{
	rows[0].set(A._11, A._21, A._31, A._41);
	rows[1].set(A._12, A._22, A._32, A._42);
	if (cls == RC_2x4)
		return 2;
	rows[2].set(A._13, A._23, A._33, A._43);
	if (cls == RC_3x4)
		return 3;
	rows[3].set(A._14, A._24, A._34, A._44);
	return 4;
}

IC void dx10ConstantBuffer::set(R_constant* C, R_constant_load& L, const Fmatrix& A)
{
	VERIFY(RC_float == C->type);
	VERIFY(RC_2x4 == L.cls || RC_3x4 == L.cls || RC_4x4 == L.cls);
	Fvector4 rows[4];
	const u32 n = dx10ConstantBuffer_rows(L.cls, A, rows);
	Write(L.index, rows, n * lineSize);
}

IC void dx10ConstantBuffer::set(R_constant* C, R_constant_load& L, const Fvector4& A)
{
	VERIFY(RC_float == C->type);
	VERIFY(RC_1x4 == L.cls || RC_1x3 == L.cls || RC_1x2 == L.cls || RC_1x1 == L.cls);
	//Fvector4*	it	= Access(L.index);
	//it->set	(A);

	u32 count = 4;
	if (L.cls == RC_1x1)
		count = 1;
	else if (L.cls == RC_1x2)
		count = 2;
	else if (L.cls == RC_1x3)
		count = 3;
	Write(L.index, &A[0], count * sizeof(float));
}

IC void dx10ConstantBuffer::set(R_constant* C, R_constant_load& L, float A)
{
	VERIFY(RC_float == C->type);
	VERIFY(RC_1x1 == L.cls);
	Write(L.index, &A, sizeof(float));
}

IC void dx10ConstantBuffer::set(R_constant* C, R_constant_load& L, int A)
{
	VERIFY(RC_int == C->type);
	VERIFY(RC_1x1 == L.cls);
	Write(L.index, &A, sizeof(int));
}

IC void dx10ConstantBuffer::seta(R_constant* C, R_constant_load& L, u32 e, const Fmatrix& A)
{
	VERIFY(RC_float == C->type);
	VERIFY(RC_2x4 == L.cls || RC_3x4 == L.cls || RC_4x4 == L.cls);
	Fvector4 rows[4];
	const u32 n = dx10ConstantBuffer_rows(L.cls, A, rows);
	Write((u32)L.index + n * lineSize * e, rows, n * lineSize);
}

IC void dx10ConstantBuffer::seta(R_constant* C, R_constant_load& L, u32 e, const Fvector4& A)
{
	VERIFY(RC_float == C->type);
	VERIFY(RC_1x4 == L.cls || RC_1x3 == L.cls || RC_1x2 == L.cls);
	Write((u32)L.index + lineSize * e, &A, lineSize);
}

IC void dx10ConstantBuffer::set_bytes(R_constant_load& L, const void* data, u32 bytes)
{
	VERIFY((u32)L.index + L.size <= m_uiBufferSize);
	BYTE* it = (BYTE*)Access(L.index);
	const u32 copied = _min(bytes, L.size);
	CopyMemory(it, data, copied);
	ZeroMemory(it + copied, L.size - copied);
}

IC void* dx10ConstantBuffer::AccessDirect(R_constant_load& L, u32 DataSize)
{
	//	Check buffer size in client code: don't know if actual data will cross
	//	buffer boundaries.
	VERIFY(L.index<(int)m_uiBufferSize);
	BYTE* res = ((BYTE*)m_pBufferData) + L.index;

	if ((u32)L.index + DataSize <= m_uiBufferSize)
	{
		m_bChanged = true;
		if (m_bDirectOnly)
			m_uiDirectEnd = L.index + DataSize;
		return res;
	}
	else return 0;
}

#endif	//	dx10ConstantBuffer_impl_included
