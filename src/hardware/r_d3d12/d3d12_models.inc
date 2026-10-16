// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file d3d12_models.inc
/// \brief [UWP]: Arena-backed models, mesh indices and sky dome draws

// Models and sky retain upload-arena allocations; replacement is fence retired.
struct ModelVertex
{
	float x, y, z, nx, ny, nz, s, t;
};
static_assert(sizeof(ModelVertex) == 32, "The model layout's stride is 32");
struct StaticBuffer
{
	StaticAllocation allocation = {};
	UINT bytes = 0;
	explicit operator bool() const { return allocation.resource != nullptr; }
};
StaticBuffer g_fanIndices, g_skyBuffer;
constexpr UINT kMaxFanVertices = 65535;
std::unordered_map<unsigned int, StaticBuffer> g_modelBuffers;
unsigned int g_nextModelBuffer = 1;
std::unordered_map<const void*, StaticBuffer> g_meshIndexBuffers;
std::vector<ModelVertex> g_modelVertices;

bool CreateStaticBuffer(const void* data, UINT bytes, StaticBuffer& buffer)
{
	RetireStatic(buffer.allocation);
	buffer = {};
	if (!bytes || !data)
		return false;
	buffer.allocation = AllocateStatic(data, bytes);
	if (!buffer)
		return false;
	buffer.bytes = bytes;
	return true;
}

bool EnsureFanIndices()
{
	if (g_fanIndices)
		return true;
	std::vector<UINT16> fan;
	fan.reserve((kMaxFanVertices - 2) * 3);
	for (UINT i = 1; i + 1 < kMaxFanVertices; ++i)
	{
		fan.push_back(0);
		fan.push_back((UINT16) i);
		fan.push_back((UINT16) (i + 1));
	}
	return CreateStaticBuffer(fan.data(), (UINT) (fan.size() * sizeof(UINT16)), g_fanIndices);
}

StaticBuffer* GetMeshIndices(const mesh_t* mesh)
{
	if (!mesh->indices)
		return nullptr;
	auto it = g_meshIndexBuffers.find(mesh->indices);
	if (it != g_meshIndexBuffers.end())
		return &it->second;
	StaticBuffer buffer;
	if (!CreateStaticBuffer(mesh->indices, mesh->numTriangles * 3 * sizeof(UINT16), buffer))
		return nullptr;
	return &g_meshIndexBuffers.emplace(mesh->indices, buffer).first->second;
}

void ReleaseModelBuffers()
{
	RetireStatic(g_fanIndices.allocation);
	g_fanIndices = {};
	RetireStatic(g_skyBuffer.allocation);
	g_skyBuffer = {};
	for (auto& entry : g_modelBuffers)
		RetireStatic(entry.second.allocation);
	for (auto& entry : g_meshIndexBuffers)
		RetireStatic(entry.second.allocation);
	g_modelBuffers.clear();
	g_meshIndexBuffers.clear();
	g_modelVertices.clear();
}

void FillVertices(int count, const float* vertices, const float* normals, const float* uvs)
{
	g_modelVertices.resize(count);
	for (int i = 0; i < count; i++)
	{
		ModelVertex& v = g_modelVertices[i];
		v.x = vertices[i * 3];
		v.y = vertices[i * 3 + 1];
		v.z = vertices[i * 3 + 2];
		v.nx = normals ? normals[i * 3] : 0.0f;
		v.ny = normals ? normals[i * 3 + 1] : 0.0f;
		v.nz = normals ? normals[i * 3 + 2] : 1.0f;
		v.s = uvs ? uvs[i * 2] : 0.0f;
		v.t = uvs ? uvs[i * 2 + 1] : 0.0f;
	}
}

void FillTinyVertices(int count, const short* vertices, const char* normals, const float* uvs)
{
	g_modelVertices.resize(count);
	for (int i = 0; i < count; i++)
	{
		ModelVertex& v = g_modelVertices[i];
		v.x = vertices[i * 3];
		v.y = vertices[i * 3 + 1];
		v.z = vertices[i * 3 + 2];
		v.nx = normals ? normals[i * 3] : 0.0f;
		v.ny = normals ? normals[i * 3 + 1] : 0.0f;
		v.nz = normals ? normals[i * 3 + 2] : 1.0f;
		v.s = uvs ? uvs[i * 2] : 0.0f;
		v.t = uvs ? uvs[i * 2 + 1] : 0.0f;
	}
}

// Upload fallback vertices before applying state: either allocation can submit a list.
void DrawMesh(const StaticBuffer* buffer, const mesh_t* mesh, bool indexed, UINT count)
{
	if (!CanDraw() || !count)
		return;
	StaticBuffer* indices = indexed ? GetMeshIndices(mesh) : nullptr;
	if (indexed && !indices)
		return;
	UploadAllocation upload = {};
	D3D12_VERTEX_BUFFER_VIEW vb = {};
	if (buffer)
		vb = {buffer->allocation.gpu, buffer->bytes, sizeof(ModelVertex)};
	else
	{
		const size_t bytes = g_modelVertices.size() * sizeof(ModelVertex);
		upload = g_vertexRing.Alloc(bytes, sizeof(ModelVertex));
		if (!upload.cpu)
			return;
		memcpy(upload.cpu, g_modelVertices.data(), bytes);
		vb = {upload.gpu, (UINT) bytes, sizeof(ModelVertex)};
	}
	if (!ApplyState(LAYOUT_MODEL, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST))
		return;
	if (!buffer && !g_vertexRing.Touch(upload, vb.SizeInBytes))
		return;
	if (!g_constantRing.Touch(g_constantAllocation, 512))
		return;
	Core_List()->IASetVertexBuffers(0, 1, &vb);
	if (indexed)
	{
		const D3D12_INDEX_BUFFER_VIEW ib = {indices->allocation.gpu, indices->bytes, DXGI_FORMAT_R16_UINT};
		Core_List()->IASetIndexBuffer(&ib);
	}
	Core_FlushBarriers();
	if (indexed)
	{
		Core_List()->DrawIndexedInstanced(count, 1, 0, 0, 0);
		g_stats.indices += count;
	}
	else
	{
		Core_List()->DrawInstanced(count, 1, 0, 0);
		g_stats.vertices += count;
	}
	g_stats.draws++;
	CaptureSurfaceProbe("model", nullptr, 0);
}

void D3D_DrawModel(
	model_t* model,
	INT32 frameIndex,
	float duration,
	float tics,
	INT32 nextFrameIndex,
	FTransform* pos,
	float hscale,
	float vscale,
	UINT8 flipped,
	UINT8 hflipped,
	FSurfaceInfo* Surface
)
{
	static float poly[4] = {0, 0, 0, 0};
	static float tint[4] = {0, 0, 0, 0};
	static float fade[4] = {0, 0, 0, 0};

	float pol = 0.0f;
	bool lit = false;

	if (!CanDraw() || model->numMeshes <= 0)
		return;

	// Affect input model scaling
	hscale *= 0.5f;
	vscale *= 0.5f;
	float scalex = hscale;
	float scaley = vscale;
	float scalez = hscale;

	if (duration > 0.0 && tics >= 0.0) // don't interpolate if instantaneous or infinite in length
	{
		float newtime = (duration - tics); // + 1;

		pol = newtime / duration;

		if (pol > 1.0f)
			pol = 1.0f;

		if (pol < 0.0f)
			pol = 0.0f;
	}

	poly[0] = Surface->PolyColor.s.red / 255.0f;
	poly[1] = Surface->PolyColor.s.green / 255.0f;
	poly[2] = Surface->PolyColor.s.blue / 255.0f;
	poly[3] = Surface->PolyColor.s.alpha / 255.0f;

	if (g_modelLighting)
	{
		if (!g_shadersEnabled)
		{
			// Because otherwise, scaling the screen negatively vertically breaks the lighting:
			// GL_LIGHT0 at (0, 1, 0, 0), a direction, which the modelview matrix takes to eye space
			const Mat4& modelview = g_modelview.back();
			float x = modelview.m[4], y = modelview.m[5], z = modelview.m[6];
			const float length = sqrtf(x * x + y * y + z * z);

			if (length > 0.0f)
			{
				x /= length;
				y /= length;
				z /= length;
			}
			g_gl.lightDir[0] = x;
			g_gl.lightDir[1] = y;
			g_gl.lightDir[2] = z;
			memcpy(g_gl.material, poly, sizeof g_gl.material);
			g_gl.lighting = lit = true;
		}
	}
	else
		SetColor(&Surface->PolyColor.s.red);

	tint[0] = Surface->TintColor.s.red / 255.0f;
	tint[1] = Surface->TintColor.s.green / 255.0f;
	tint[2] = Surface->TintColor.s.blue / 255.0f;
	tint[3] = Surface->TintColor.s.alpha / 255.0f;

	fade[0] = Surface->FadeColor.s.red / 255.0f;
	fade[1] = Surface->FadeColor.s.green / 255.0f;
	fade[2] = Surface->FadeColor.s.blue / 255.0f;
	fade[3] = Surface->FadeColor.s.alpha / 255.0f;

	FBITFIELD flags = (Surface->PolyFlags | PF_Modulated);
	if (Surface->PolyFlags & (PF_Additive | PF_Subtractive | PF_ReverseSubtract | PF_Multiplicative))
		flags |= PF_Occlude;
	else if (Surface->PolyColor.s.alpha == 0xFF)
		flags |= (PF_Occlude | PF_Masked);

	D3D_SetBlend(flags);
	SetUniforms(Surface, poly, tint, fade);
	SetLightTable(Surface->LightTableId);

	g_gl.cullFace = true;

	// flipped is if the object is vertically flipped
	// hflipped is if the object is horizontally flipped
	// pos->flip is if the screen is flipped vertically
	// pos->mirror is if the screen is flipped horizontally
	// XOR all the flips together to figure out what culling to use!
	g_gl.cullFront = (flipped ^ hflipped ^ pos->flip ^ pos->mirror) != 0;

	MatPush(); // should be the same as glLoadIdentity
	MatTranslate(pos->x, pos->z, pos->y);
	if (flipped)
		scaley = -scaley;
	if (hflipped)
		scalez = -scalez;

	MatRotate(pos->anglez, 0.0f, 0.0f, -1.0f);
	MatRotate(pos->anglex, 1.0f, 0.0f, 0.0f);
	MatRotate(pos->angley, 0.0f, -1.0f, 0.0f);

	if (pos->roll)
	{
		MatTranslate(pos->centerx, pos->centery, 0);
		MatRotate(pos->rollangle, pos->rollx, 0.0f, pos->rollz);
		MatTranslate(-pos->centerx, -pos->centery, 0);
	}

	MatScale(scalex, scaley, scalez);

	const bool useTinyFrames = model->meshes[0].tinyframes != NULL;

	if (useTinyFrames)
		MatScale(1 / 64.0f, 1 / 64.0f, 1 / 64.0f);

	// Don't use the VBO if it does not have the correct texture coordinates.
	// (Can happen when model uses a sprite as a texture and the sprite changes)
	const bool useVBO = memcmp(&(model->vbo_max_s), &(model->max_s), sizeof(model->max_s)) == 0 &&
						memcmp(&(model->vbo_max_t), &(model->max_t), sizeof(model->max_t)) == 0;

	for (int i = 0; i < model->numMeshes; i++)
	{
		mesh_t* mesh = &model->meshes[i];

		if (mesh->numFrames <= 0)
			continue;

		if (useTinyFrames)
		{
			tinyframe_t* frame = &mesh->tinyframes[frameIndex % mesh->numFrames];
			tinyframe_t* nextframe = NULL;

			if (nextFrameIndex != -1)
				nextframe = &mesh->tinyframes[nextFrameIndex % mesh->numFrames];

			if (!nextframe || pol == 0.0f)
			{
				auto buffer = useVBO ? g_modelBuffers.find(frame->vboID) : g_modelBuffers.end();
				if (buffer != g_modelBuffers.end() && buffer->second)
					DrawMesh(&buffer->second, mesh, true, mesh->numTriangles * 3);
				else
				{
					g_stats.modelFallbacks++;
					FillTinyVertices(mesh->numVertices, frame->vertices, frame->normals, mesh->uvs);
					DrawMesh(nullptr, mesh, true, mesh->numTriangles * 3);
				}
			}
			else
			{
				g_stats.modelLerps++;
				g_modelVertices.resize(mesh->numVertices);
				for (int j = 0; j < mesh->numVertices; j++)
				{
					ModelVertex& v = g_modelVertices[j];
					float* position = &v.x;
					float* normal = &v.nx;

					for (int k = 0; k < 3; k++)
					{
						const int c = j * 3 + k;
						// Interpolate
						position[k] =
							(short) (frame->vertices[c] + (pol * (nextframe->vertices[c] - frame->vertices[c])));
						normal[k] = (char) (frame->normals[c] + (pol * (nextframe->normals[c] - frame->normals[c])));
					}
					v.s = mesh->uvs[j * 2];
					v.t = mesh->uvs[j * 2 + 1];
				}
				DrawMesh(nullptr, mesh, true, mesh->numTriangles * 3);
			}
		}
		else
		{
			mdlframe_t* frame = &mesh->frames[frameIndex % mesh->numFrames];
			mdlframe_t* nextframe = NULL;

			if (nextFrameIndex != -1)
				nextframe = &mesh->frames[nextFrameIndex % mesh->numFrames];

			if (!nextframe || pol == 0.0f)
			{
				auto buffer = useVBO ? g_modelBuffers.find(frame->vboID) : g_modelBuffers.end();
				if (buffer != g_modelBuffers.end() && buffer->second)
					DrawMesh(&buffer->second, mesh, false, mesh->numTriangles * 3);
				else
				{
					g_stats.modelFallbacks++;
					FillVertices(mesh->numTriangles * 3, frame->vertices, frame->normals, mesh->uvs);
					DrawMesh(nullptr, mesh, false, mesh->numTriangles * 3);
				}
			}
			else
			{
				g_stats.modelLerps++;
				g_modelVertices.resize(mesh->numVertices);
				for (int j = 0; j < mesh->numVertices; j++)
				{
					ModelVertex& v = g_modelVertices[j];
					float* position = &v.x;
					float* normal = &v.nx;

					for (int k = 0; k < 3; k++)
					{
						const int c = j * 3 + k;
						// Interpolate
						position[k] = frame->vertices[c] + (pol * (nextframe->vertices[c] - frame->vertices[c]));
						normal[k] = frame->normals[c] + (pol * (nextframe->normals[c] - frame->normals[c]));
					}
					v.s = mesh->uvs[j * 2];
					v.t = mesh->uvs[j * 2 + 1];
				}
				DrawMesh(nullptr, mesh, false, mesh->numVertices);
			}
		}
	}

	MatPop(); // should be the same as glLoadIdentity
	g_gl.cullFace = false;

	if (lit)
		g_gl.lighting = false;

	g_stats.models++;
}

void D3D_CreateModelVBOs(model_t* model)
{
	int buffers = 0;

	if (!Core_Device())
		return;

	for (int i = 0; i < model->numMeshes; i++)
	{
		mesh_t* mesh = &model->meshes[i];

		for (int j = 0; j < mesh->numFrames; j++)
		{
			unsigned int* vboID;

			if (mesh->frames)
			{
				mdlframe_t* frame = &mesh->frames[j];
				FillVertices(mesh->numTriangles * 3, frame->vertices, frame->normals, mesh->uvs);
				vboID = &frame->vboID;
			}
			else if (mesh->tinyframes)
			{
				tinyframe_t* frame = &mesh->tinyframes[j];
				FillTinyVertices(mesh->numVertices, frame->vertices, frame->normals, mesh->uvs);
				vboID = &frame->vboID;
			}
			else
				break;

			if (*vboID)
			{
				auto old = g_modelBuffers.find(*vboID);
				if (old != g_modelBuffers.end())
				{
					RetireStatic(old->second.allocation);
					g_modelBuffers.erase(old);
				}
			}
			*vboID = 0;

			StaticBuffer buffer;
			if (CreateStaticBuffer(
					g_modelVertices.data(),
					(UINT) (g_modelVertices.size() * sizeof(ModelVertex)),
					buffer
				))
			{
				*vboID = g_nextModelBuffer++;
				g_modelBuffers[*vboID] = buffer;
				buffers++;
			}
		}
	}

	Log("made %d vertex buffers for model %s", buffers, model->mdlFilename ? model->mdlFilename : "?");
}

void D3D_RenderSkyDome(gl_sky_t* sky)
{
	SetUniforms(nullptr, nullptr, nullptr, nullptr);
	if (!CanDraw())
		return;
	if (sky->rebuild || !g_skyBuffer)
	{
		if (!CreateStaticBuffer(sky->data, (UINT) (sky->vertex_count * sizeof(sky->data[0])), g_skyBuffer))
			return;
		sky->vbo = 1;
		sky->rebuild = false;
		g_stats.skyBuilds++;
		D3D12_LOG_ONCE("built a sky dome of %d vertices in %d loops", sky->vertex_count, sky->loopcount);
	}
	MatScale(1.0f, (float) sky->height / 200.0f, 1.0f);
	MatRotate(270.0f, 0.0f, 1.0f, 0.0f);
	for (int j = 0; j < 2; ++j)
		for (int i = 0; i < sky->loopcount; ++i)
		{
			const gl_skyloopdef_t* loop = &sky->loops[i];
			if (j == 0 ? loop->use_texture : !loop->use_texture)
				continue;
			const bool fan = loop->mode == HWD_SKYLOOP_FAN;
			if (fan ? (loop->vertexcount < 3 || (UINT) loop->vertexcount > kMaxFanVertices)
					: loop->mode != HWD_SKYLOOP_STRIP)
				continue;
			if (fan && !EnsureFanIndices())
				continue;
			if (!ApplyState(
					LAYOUT_SKY,
					fan ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
				))
				continue;
			if (!g_constantRing.Touch(g_constantAllocation, 512))
				continue;
			const D3D12_VERTEX_BUFFER_VIEW vb = {g_skyBuffer.allocation.gpu, g_skyBuffer.bytes, sizeof(gl_skyvertex_t)};
			Core_List()->IASetVertexBuffers(0, 1, &vb);
			if (fan)
			{
				const D3D12_INDEX_BUFFER_VIEW ib =
					{g_fanIndices.allocation.gpu, g_fanIndices.bytes, DXGI_FORMAT_R16_UINT};
				Core_List()->IASetIndexBuffer(&ib);
			}
			Core_FlushBarriers();
			if (fan)
				Core_List()->DrawIndexedInstanced((loop->vertexcount - 2) * 3, 1, 0, loop->vertexindex, 0);
			else
				Core_List()->DrawInstanced(loop->vertexcount, 1, loop->vertexindex, 0);
			g_stats.draws++;
			g_stats.vertices += loop->vertexcount;
			CaptureSurfaceProbe("sky", nullptr, 0);
		}
	SetColor(kWhite);
	g_stats.skies++;
}
