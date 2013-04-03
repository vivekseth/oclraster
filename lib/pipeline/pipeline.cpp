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

#include "pipeline.h"
#include "oclraster.h"

#if defined(OCLRASTER_IOS)
#include "ios_helper.h"
#endif

pipeline::pipeline() :
default_framebuffer(0, 0),
event_handler_fnctr(this, &pipeline::event_handler) {
	create_framebuffers(size2(oclraster::get_width(), oclraster::get_height()));
	state.framebuffer_size = default_framebuffer.get_size();
	state.active_framebuffer = &default_framebuffer;
	oclraster::get_event()->add_internal_event_handler(event_handler_fnctr, EVENT_TYPE::WINDOW_RESIZE, EVENT_TYPE::KERNEL_RELOAD);
	
#if defined(OCLRASTER_IOS)
	static const float fullscreen_triangle[6] { 1.0f, 1.0f, 1.0f, -3.0f, -3.0f, 1.0f };
	glGenBuffers(1, &vbo_fullscreen_triangle);
	glBindBuffer(GL_ARRAY_BUFFER, vbo_fullscreen_triangle);
	glBufferData(GL_ARRAY_BUFFER, 6 * sizeof(float), fullscreen_triangle, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
#endif
}

pipeline::~pipeline() {
	oclraster::get_event()->remove_event_handler(event_handler_fnctr);
	
	destroy_framebuffers();
	
#if defined(OCLRASTER_IOS)
	if(glIsBuffer(vbo_fullscreen_triangle)) glDeleteBuffers(1, &vbo_fullscreen_triangle);
#endif
}

bool pipeline::event_handler(EVENT_TYPE type, shared_ptr<event_object> obj) {
	if(type == EVENT_TYPE::WINDOW_RESIZE) {
		const window_resize_event& evt = (const window_resize_event&)*obj;
		create_framebuffers(evt.size);
	}
	else if(type == EVENT_TYPE::KERNEL_RELOAD) {
		// unbind user programs, since those are invalid now
		state.transform_prog = nullptr;
		state.rasterize_prog = nullptr;
	}
	return true;
}

void pipeline::create_framebuffers(const uint2& size) {
	const bool is_default_framebuffer = (state.active_framebuffer == &default_framebuffer);
	
	// destroy old framebuffers first
	destroy_framebuffers();
	
	const uint2 scaled_size = float2(size) / oclraster::get_upscaling();
	oclr_debug("size: %v -> %v", size, scaled_size);
	
	//
	default_framebuffer = framebuffer::create_with_images(scaled_size.x, scaled_size.y,
														  { { IMAGE_TYPE::UINT_8, IMAGE_CHANNEL::RGBA } },
														  { IMAGE_TYPE::FLOAT_32, IMAGE_CHANNEL::R });
	
	// create a fbo for copying the color framebuffer every frame and displaying it
	// (there is no other way, unfortunately)
	glGenFramebuffers(1, &copy_fbo_id);
	glBindFramebuffer(GL_FRAMEBUFFER, copy_fbo_id);
	glGenTextures(1, &copy_fbo_tex_id);
	glBindTexture(GL_TEXTURE_2D, copy_fbo_tex_id);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, rtt::convert_internal_format(GL_RGBA8), scaled_size.x, scaled_size.y,
				 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, copy_fbo_tex_id, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, OCLRASTER_DEFAULT_FRAMEBUFFER);
	glBindTexture(GL_TEXTURE_2D, 0);
	
	// rebind new default framebuffer (+set correct state)
	if(is_default_framebuffer) {
		bind_framebuffer(nullptr);
	}
	
	// do an initial clear
	default_framebuffer.clear();
}

void pipeline::destroy_framebuffers() {
	framebuffer::destroy_images(default_framebuffer);
	
	glBindFramebuffer(GL_FRAMEBUFFER, OCLRASTER_DEFAULT_FRAMEBUFFER);
	glBindTexture(GL_TEXTURE_2D, 0);
	if(copy_fbo_tex_id != 0) glDeleteTextures(1, &copy_fbo_tex_id);
	if(copy_fbo_id != 0) glDeleteFramebuffers(1, &copy_fbo_id);
	copy_fbo_tex_id = 0;
	copy_fbo_id = 0;
}

void pipeline::swap() {
	// draw/blit to screen
#if defined(OCLRASTER_IOS)
	glBindFramebuffer(GL_FRAMEBUFFER, OCLRASTER_DEFAULT_FRAMEBUFFER);
#endif
	oclraster::start_2d_draw();
	
	// copy opencl framebuffer to blit framebuffer/texture
	const uint2 default_fb_size = default_framebuffer.get_size();
	image* fbo_img = default_framebuffer.get_image(0);
	auto fbo_data = fbo_img->map(opencl::MAP_BUFFER_FLAG::READ | opencl::MAP_BUFFER_FLAG::BLOCK);
#if !defined(OCLRASTER_IOS)
	glBindFramebuffer(GL_FRAMEBUFFER, copy_fbo_id);
#endif
	glBindTexture(GL_TEXTURE_2D, copy_fbo_tex_id);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, default_fb_size.x, default_fb_size.y,
					GL_RGBA, GL_UNSIGNED_BYTE, (const unsigned char*)fbo_data);
	fbo_img->unmap(fbo_data);
	
#if !defined(OCLRASTER_IOS)
	// blit
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, OCLRASTER_DEFAULT_FRAMEBUFFER);
	glBlitFramebuffer(0, 0, default_fb_size.x, default_fb_size.y,
					  0, 0, oclraster::get_width(), oclraster::get_height(),
					  GL_COLOR_BUFFER_BIT, GL_NEAREST);
#else
	// draw
	shader_object* shd = ios_helper::get_shader("BLIT");
	glUseProgram(shd->program.program);
	glUniform1i(shd->program.uniforms.find("tex")->second.location, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, copy_fbo_tex_id);
	
	glFrontFace(GL_CW);
	glBindBuffer(GL_ARRAY_BUFFER, vbo_fullscreen_triangle);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDisableVertexAttribArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glFrontFace(GL_CCW);
	
	glUseProgram(0);
#endif
	
	oclraster::stop_2d_draw();
	
#if !defined(OCLRASTER_IOS)
	glBindFramebuffer(GL_READ_FRAMEBUFFER, OCLRASTER_DEFAULT_FRAMEBUFFER);
	glBindTexture(GL_TEXTURE_2D, 0);
#endif
	
	default_framebuffer.clear();
}

void pipeline::draw(const pair<unsigned int, unsigned int> element_range) {
#if defined(OCLRASTER_DEBUG)
	// TODO: check buffer sanity/correctness (in debug mode)
#endif
	
	const auto index_count = (element_range.second - element_range.first + 1) * 3;
	const auto num_elements = element_range.second - element_range.first + 1;
	state.triangle_count = num_elements;
	/*const auto num_elements = (element_range.first != ~0u ?
							   element_range.second - element_range.first + 1 :
							   ib.index_count / 3);*/
	
	// initialize draw state
	state.depth_test = 1;
	state.bin_count = {
		(state.framebuffer_size.x / state.bin_size.x) + ((state.framebuffer_size.x % state.bin_size.x) != 0 ? 1 : 0),
		(state.framebuffer_size.y / state.bin_size.y) + ((state.framebuffer_size.y % state.bin_size.y) != 0 ? 1 : 0)
	};
	state.batch_count = ((state.triangle_count / state.batch_size) +
						 ((state.triangle_count % state.batch_size) != 0 ? 1 : 0));
	
	// TODO: this should be static!
	// note: internal transformed buffer size must be a multiple of "batch size" triangles (necessary for the binner)
	const unsigned int tc_mod_batch_size = (state.triangle_count % OCLRASTER_BATCH_SIZE);
	state.transformed_buffer = ocl->create_buffer(opencl::BUFFER_FLAG::READ_WRITE,
												  state.transformed_primitive_size *
												  (state.triangle_count + (tc_mod_batch_size == 0 ? 0 :
																		   OCLRASTER_BATCH_SIZE - tc_mod_batch_size)));
	
	// create user transformed buffers (transform program outputs)
	const auto active_device = ocl->get_active_device();
	for(const auto& tp_struct : state.transform_prog->get_structs()) {
		if(tp_struct.type == oclraster_program::STRUCT_TYPE::OUTPUT) {
			opencl::buffer_object* buffer = ocl->create_buffer(opencl::BUFFER_FLAG::READ_WRITE,
															   // get device specific size from program
															   tp_struct.device_infos.at(active_device).struct_size * index_count);
			state.user_transformed_buffers.push_back(buffer);
			bind_buffer(tp_struct.object_name, *buffer);
		}
	}
	
	// pipeline
	transform.transform(state, state.triangle_count);
	const auto queue_buffer = binning.bin(state);
	
	// TODO: pipelining/splitting
	rasterization.rasterize(state, queue_buffer);
	
	//
	ocl->delete_buffer(state.transformed_buffer);
	
	// delete user transformed buffers
	for(const auto& ut_buffer : state.user_transformed_buffers) {
		ocl->delete_buffer(ut_buffer);
	}
	state.user_transformed_buffers.clear();
}

void pipeline::bind_buffer(const string& name, const opencl_base::buffer_object& buffer) {
	const auto existing_buffer = state.user_buffers.find(name);
	if(existing_buffer != state.user_buffers.cend()) {
		state.user_buffers.erase(existing_buffer);
	}
	state.user_buffers.emplace(name, buffer);
}

void pipeline::bind_image(const string& name, const image& img) {
	const auto existing_image = state.user_images.find(name);
	if(existing_image != state.user_images.cend()) {
		state.user_images.erase(existing_image);
	}
	state.user_images.emplace(name, img);
}

void pipeline::bind_framebuffer(framebuffer* fb) {
	if(fb == nullptr) {
		state.active_framebuffer = &default_framebuffer;
	}
	else state.active_framebuffer = fb;
	
	//
	const auto new_fb_size = state.active_framebuffer->get_size();
	if(new_fb_size.x != state.framebuffer_size.x ||
	   new_fb_size.y != state.framebuffer_size.y) {
		state.framebuffer_size = new_fb_size;
	}
}

const framebuffer* pipeline::get_default_framebuffer() const {
	return &default_framebuffer;
}
