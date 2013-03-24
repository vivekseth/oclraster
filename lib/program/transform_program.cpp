/*
 *  Flexible OpenCL Rasterizer (oclraster)
 *  Copyright (C) 2012 - 2013 Florian Ziesche
 *  
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License only.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "transform_program.h"
#include "image.h"

#if defined(OCLRASTER_INTERNAL_PROGRAM_DEBUG)
string template_transform_program { "" };
#else
// awesome raw string literals are awesome
static constexpr char template_transform_program[] { u8R"OCLRASTER_RAWSTR(
	#include "oclr_global.h"
	#include "oclr_math.h"
	#include "oclr_matrix.h"
	#include "oclr_image.h"
	
	typedef struct __attribute__((packed, aligned(16))) {
		float4 camera_position;
		float4 camera_origin;
		float4 camera_x_vec;
		float4 camera_y_vec;
		float4 camera_forward;
		float4 frustum_normals[3];
		uint2 viewport;
	} constant_data;
	
	typedef struct __attribute__((packed, aligned(16))) {
		// VV0: 0 - 2
		// VV1: 3 - 5
		// VV2: 6 - 8
		// depth: 9
		// cam relation: 10 - 12
		// unused: 13 - 15
		float data[16];
	} transformed_data;
	
	typedef struct __attribute__((packed, aligned(16))) {
		unsigned int triangle_count;
	} info_buffer_data;
	
	// transform rerouting
	#define transform(vertex) _oclraster_transform(vertex, VE, transformed_vertex)
	OCLRASTER_FUNC void _oclraster_transform(float4 vertex, const float3* VE, float3* transformed_vertex) {
		*transformed_vertex = vertex.xyz - *VE;
	}
	
	//###OCLRASTER_USER_CODE###
	
	//
	kernel void _oclraster_program(//###OCLRASTER_USER_STRUCTS###
								   global const unsigned int* index_buffer,
								   global transformed_data* transformed_buffer,
								   global info_buffer_data* info_buffer,
								   constant constant_data* cdata,
								   const unsigned int triangle_count) {
		const unsigned int triangle_id = get_global_id(0);
		// global work size is greater than the actual triangle count
		// -> check for triangle_count instead of get_global_size(0)
		if(triangle_id >= triangle_count) return;
		
		const unsigned int indices[3] = {
			index_buffer[triangle_id*3],
			index_buffer[triangle_id*3 + 1],
			index_buffer[triangle_id*3 + 2]
		};
		
		//
		const float3 D0 = cdata->camera_origin.xyz;
		const float3 DX = cdata->camera_x_vec.xyz;
		const float3 DY = cdata->camera_y_vec.xyz;
		const float3 VE = cdata->camera_position.xyz;
		const float3 forward = cdata->camera_forward.xyz;
		
		float3 vertices[3];
		//###OCLRASTER_USER_PRE_MAIN_CALL###
		for(int i = 0; i < 3; i++) {
			//###OCLRASTER_USER_MAIN_CALL###
		}
		
		// if component < 0 => vertex is behind cam, == 0 => on the near plane, > 0 => in front of the cam
		const float triangle_cam_relation[3] = {
			dot(vertices[0], forward),
			dot(vertices[1], forward),
			dot(vertices[2], forward)
		};
		
		// if xyz < 0, don't add the triangle in the first place
		if(triangle_cam_relation[0] < 0.0f &&
		   triangle_cam_relation[1] < 0.0f &&
		   triangle_cam_relation[2] < 0.0f) {
			// all vertices are behind the camera
			return;
		}
		
		// frustum culling using the "p/n-test"
		// also thx to ryg for figuring out this optimized version of the p/n-test
		const float3 aabb_min = fmin(fmin(vertices[0], vertices[1]), vertices[2]);
		const float3 aabb_max = fmax(fmax(vertices[0], vertices[1]), vertices[2]);
		const float3 aabb_center = (aabb_max + aabb_min); // note: actual scale doesn't matter (-> no *0.5f)
		const float3 aabb_extent = (aabb_max - aabb_min); // extent must have the same scale though
		float4 fc_dot = (float4)(0.0f, 0.0f, 0.0f, 0.0f);
		for(unsigned int i = 0; i < 3; i++) {
			const float4 plane_normal = cdata->frustum_normals[i];
			const uint4 plane_sign = *(const uint4*)&plane_normal & (uint4)(0x80000000, 0x80000000, 0x80000000, 0x80000000);
			const uint4 flipped_extent = (uint4)(((const uint*)&aabb_extent)[i]) ^ plane_sign;
			const float4 dot_param = (float4)(((const float*)&aabb_center)[i]) + *(const float4*)&flipped_extent;
			fc_dot += dot_param * plane_normal;
		}
		// if any dot product is less than 0 (aabb is completely outside any plane) -> cull
		if(any(signbit(fc_dot))) {
			return;
		}
		
		// since VE0 can be interpreted as (0, 0, 0, 1) after it has been substracted from the vertices,
		// the original algorithm (requiring the computation of 4x4 matrix determinants) can be simplified:
		const float3 c01 = cross(vertices[0] - vertices[1], vertices[1]);
		const float3 c20 = cross(vertices[2] - vertices[0], vertices[0]);
		const float3 c12 = cross(vertices[1] - vertices[2], vertices[2]);
		const float o01 = dot(D0, c01);
		const float o20 = dot(D0, c20);
		const float o12 = dot(D0, c12);
		const float x01 = dot(DX, c01);
		const float x20 = dot(DX, c20);
		const float x12 = dot(DX, c12);
		const float y01 = dot(DY, c01);
		const float y20 = dot(DY, c20);
		const float y12 = dot(DY, c12);
		
		// TODO: compute triangle area through dx/dy -> vertices diff?
		
		//
		const float3 VV[3] = {
			(float3)(x12, y12, o12),
			(float3)(x20, y20, o20),
			(float3)(x01, y01, o01)
		};
		float VV_depth = dot(vertices[0], c12);
		
		// imprecision culling (this will really mess up the later pipeline if it isn't culled here)
		// also note that all values must be below the epsilon, otherwise these might be valid for clipping
#define VV_EPSILON 0.00001f
#define VV_EPSILON_9 0.00009f
		//if(fabs(VV[0].x) <= VV_EPSILON && fabs(VV[0].y) <= VV_EPSILON && fabs(VV[0].z) <= VV_EPSILON &&
		//   fabs(VV[1].x) <= VV_EPSILON && fabs(VV[1].y) <= VV_EPSILON && fabs(VV[1].z) <= VV_EPSILON &&
		//   fabs(VV[2].x) <= VV_EPSILON && fabs(VV[2].y) <= VV_EPSILON && fabs(VV[2].z) <= VV_EPSILON) {
		// TODO: dot product might also work (even lower epsilon though)
		// TODO: epsilon too high?
		/*if(((fabs(VV[0].x) + fabs(VV[0].y)) +
			(fabs(VV[0].z) + fabs(VV[1].x)) +
			(fabs(VV[1].y) + fabs(VV[1].z)) +
			(fabs(VV[2].x) + fabs(VV[2].y) + fabs(VV[2].z))) <= VV_EPSILON_9) {
			//printf("imprecision culled (tp1): %d\n", triangle_id);
			return;
		}*/
		
		// triangle area and backface culling:
		// note: in this stage, this requires the triangle to be completely visible (no clipping)
		const float fscreen_size[2] = { convert_float(cdata->viewport.x), convert_float(cdata->viewport.y) };
#define viewport_test(coord, axis) ((coord < 0.0f || coord >= fscreen_size[axis]) ? -1.0f : coord)
		float coord_xs[3];
		float coord_ys[3];
		unsigned int valid_coords = 0;
		for(unsigned int i = 0u; i < 3u; i++) {
			// { 1, 2 }, { 0, 2 }, { 0, 1 }
			const unsigned int i0 = (i == 0u ? 1u : 0u);
			const unsigned int i1 = (i == 2u ? 1u : 2u);
			
			const float d = 1.0f / (VV[i0].x * VV[i1].y - VV[i0].y * VV[i1].x);
			coord_xs[i] = (triangle_cam_relation[i] < 0.0f ? -1.0f :
						   (VV[i0].y * VV[i1].z - VV[i0].z * VV[i1].y) * d);
			coord_ys[i] = (triangle_cam_relation[i] < 0.0f ? -1.0f :
						   (VV[i0].z * VV[i1].x - VV[i0].x * VV[i1].z) * d);
			coord_xs[i] = viewport_test(coord_xs[i], 0);
			coord_ys[i] = viewport_test(coord_ys[i], 1);
			
			if(coord_xs[i] >= 0.0f && coord_ys[i] >= 0.0f) {
				valid_coords++;
			}
		}
		
		if(valid_coords == 3) {
			// compute triangle area if all vertices pass
			const float2 e0 = (float2)(coord_xs[1] - coord_xs[0],
									   coord_ys[1] - coord_ys[0]);
			const float2 e1 = (float2)(coord_xs[2] - coord_xs[0],
									   coord_ys[2] - coord_ys[0]);
			const float area = -0.5f * (e0.x * e1.y - e0.y * e1.x);
			// half sample size (TODO: -> check if between sample points; <=1/2^8 sample size seems to be a good threshold?)
			if(area < 0.00390625f) {
				return; // cull
			}
		}
		
		// second imprecision culling:
		// if all direct clip/vertex screen space coordinates are invalid or 0 -> cull
		if((coord_xs[0] == 0.0f || coord_xs[0] == -1.0f) &&
		   (coord_xs[1] == 0.0f || coord_xs[1] == -1.0f) &&
		   (coord_xs[2] == 0.0f || coord_xs[2] == -1.0f) &&
		   (coord_ys[0] == 0.0f || coord_ys[0] == -1.0f) &&
		   (coord_ys[1] == 0.0f || coord_ys[1] == -1.0f) &&
		   (coord_ys[2] == 0.0f || coord_ys[2] == -1.0f)) {
			//printf("imprecision culled (tp2): %d\n", triangle_id);
			return;
		}
		
		// output:
		const unsigned int triangle_index = atomic_inc(&info_buffer->triangle_count);
		global float* data = transformed_buffer[triangle_index].data;
		for(unsigned int i = 0u; i < 3u; i++) {
			*data++ = VV[i].x;
			*data++ = VV[i].y;
			*data++ = VV[i].z;
		}
		*data++ = VV_depth;
		*data++ = triangle_cam_relation[0];
		*data++ = triangle_cam_relation[1];
		*data++ = triangle_cam_relation[2];
		
		// note: this is isn't the most space efficient way to do this,
		// but it doesn't require any index -> triangle id mapping or
		// multiple dependent memory lookups (-> faster in the end)
		//###OCLRASTER_USER_OUTPUT_COPY###
	}
)OCLRASTER_RAWSTR"};
#endif

transform_program::transform_program(const string& code, const string entry_function_) :
oclraster_program(code, entry_function_) {
	process_program(code);
}

transform_program::~transform_program() {
}

string transform_program::specialized_processing(const string& code,
												 const kernel_image_spec& image_spec) {
	// insert (processed) user code into template program
	string program_code = template_transform_program;
	core::find_and_replace(program_code, "//###OCLRASTER_USER_CODE###", code);
	
	//
	vector<string> image_decls;
	const string kernel_parameters { create_user_kernel_parameters(image_spec, image_decls, false) };
	core::find_and_replace(program_code, "//###OCLRASTER_USER_STRUCTS###", kernel_parameters);
	
	// insert main call + prior buffer handling
	string buffer_handling_code = "";
	string pre_buffer_handling_code = "";
	string output_handling_code = "";
	string main_call_parameters = "";
	size_t cur_user_buffer = 0;
	for(const auto& oclr_struct : structs) {
		const string cur_user_buffer_str = size_t2string(cur_user_buffer);
		switch (oclr_struct.type) {
			case oclraster_program::STRUCT_TYPE::INPUT:
				buffer_handling_code += oclr_struct.name + " user_buffer_element_" + cur_user_buffer_str +
										 " = user_buffer_"+cur_user_buffer_str+"[indices[i]];\n";
				main_call_parameters += "&user_buffer_element_" + cur_user_buffer_str + ", ";
				break;
			case oclraster_program::STRUCT_TYPE::OUTPUT:
				pre_buffer_handling_code += oclr_struct.name + " user_buffer_element_" + cur_user_buffer_str + "[3];\n";
				main_call_parameters += "&user_buffer_element_" + cur_user_buffer_str + "[i], ";
				output_handling_code += "for(unsigned int i = 0; i < 3; i++) {\n";
				output_handling_code += "const unsigned int idx = (triangle_index * 3) + i;\n";
				for(const auto& var : oclr_struct.variables) {
					output_handling_code += "user_buffer_" + cur_user_buffer_str + "[idx]." + var + " = ";
					output_handling_code += "user_buffer_element_" + cur_user_buffer_str + "[i]." + var + ";\n";
				}
				output_handling_code += "}\n";
				break;
			case oclraster_program::STRUCT_TYPE::UNIFORMS:
				pre_buffer_handling_code += ("const " + oclr_struct.name + " user_buffer_element_" +
											 cur_user_buffer_str + " = *user_buffer_" + cur_user_buffer_str + ";\n");
				main_call_parameters += "&user_buffer_element_" + cur_user_buffer_str + ", ";
				break;
			case oclraster_program::STRUCT_TYPE::IMAGES:
			case oclraster_program::STRUCT_TYPE::FRAMEBUFFER: oclr_unreachable();
		}
		cur_user_buffer++;
	}
	for(const auto& img : images.image_names) {
		main_call_parameters += img + ", ";
	}
	main_call_parameters += "i, &VE, &vertices[i]"; // the same for all transform programs
	core::find_and_replace(program_code, "//###OCLRASTER_USER_PRE_MAIN_CALL###", pre_buffer_handling_code);
	core::find_and_replace(program_code, "//###OCLRASTER_USER_MAIN_CALL###",
						   buffer_handling_code+"_oclraster_user_"+entry_function+"("+main_call_parameters+");");
	core::find_and_replace(program_code, "//###OCLRASTER_USER_OUTPUT_COPY###", output_handling_code);
	
	// replace remaining image placeholders
	for(size_t i = 0, img_count = image_decls.size(); i < img_count; i++) {
		core::find_and_replace(program_code, "//###OCLRASTER_IMAGE_"+size_t2string(i)+"###", image_decls[i]);
	}
	
	// done
	//oclr_msg("generated transform user program: %s", program_code);
	return program_code;
}

string transform_program::get_fixed_entry_function_parameters() const {
	return "const int index, const float3* VE, float3* transformed_vertex";
}

string transform_program::get_qualifier_for_struct_type(const STRUCT_TYPE& type) const {
	switch(type) {
		case STRUCT_TYPE::INPUT:
		case STRUCT_TYPE::UNIFORMS:
			return "const";
		case STRUCT_TYPE::OUTPUT:
			// private memory
			return "";
		case oclraster_program::STRUCT_TYPE::IMAGES:
		case oclraster_program::STRUCT_TYPE::FRAMEBUFFER:
			return "";
	}
}
