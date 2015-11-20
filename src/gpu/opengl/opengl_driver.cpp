#include <glbinding/gl/gl.h>
#include <glbinding/Binding.h>
#include <gsl.h>
#include <fstream>

#include "gpu/commandqueue.h"
#include "gpu/pm4_buffer.h"
#include "gpu/pm4_reader.h"
#include "gpu/latte_registers.h"
#include "gpu/latte_untile.h"
#include "opengl_driver.h"
#include "utils/log.h"

namespace gpu
{

namespace opengl
{

bool GLDriver::checkReadyDraw()
{
   if (!checkActiveShader()) {
      gLog->warn("Skipping draw with invalid shader.");
      return false;
   }

   if (!checkActiveUniforms()) {
      gLog->warn("Skipping draw with invalid uniforms.");
      return false;
   }

   if (!checkActiveColorBuffer()) {
      gLog->warn("Skipping draw with invalid color buffer.");
      return false;
   }

   if (!checkActiveDepthBuffer()) {
      gLog->warn("Skipping draw with invalid depth buffer.");
      return false;
   }

   return true;
}

ColorBuffer * 
GLDriver::getColorBuffer(latte::CB_COLORN_BASE &cb_color_base,
                         latte::CB_COLORN_SIZE &cb_color_size, 
                         latte::CB_COLORN_INFO &cb_color_info)
{
   auto buffer = &mColorBuffers[cb_color_base.BASE_256B];
   buffer->cb_color_base = cb_color_base;

   if (!buffer->object) {
      auto format = cb_color_info.FORMAT;
      auto pitch_tile_max = cb_color_size.PITCH_TILE_MAX;
      auto slice_tile_max = cb_color_size.SLICE_TILE_MAX;

      auto pitch = gsl::narrow_cast<gl::GLsizei>((pitch_tile_max + 1) * latte::tile_width);
      auto height = gsl::narrow_cast<gl::GLsizei>(((slice_tile_max + 1) * (latte::tile_width * latte::tile_height)) / pitch);

      // Create color buffer
      gl::glGenTextures(1, &buffer->object);
      gl::glBindTexture(gl::GL_TEXTURE_2D, buffer->object);
      gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAG_FILTER, static_cast<int>(gl::GL_NEAREST));
      gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MIN_FILTER, static_cast<int>(gl::GL_NEAREST));
      gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_WRAP_S, static_cast<int>(gl::GL_CLAMP_TO_EDGE));
      gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_WRAP_T, static_cast<int>(gl::GL_CLAMP_TO_EDGE));
      gl::glTexImage2D(gl::GL_TEXTURE_2D, 0, static_cast<int>(gl::GL_RGBA), pitch, height, 0, gl::GL_RGBA, gl::GL_UNSIGNED_BYTE, 0);
   }

   return buffer;
}

DepthBuffer *
GLDriver::getDepthBuffer(latte::DB_DEPTH_BASE &db_depth_base,
   latte::DB_DEPTH_SIZE &db_depth_size,
   latte::DB_DEPTH_INFO &db_depth_info)
{
   auto buffer = &mDepthBuffers[db_depth_base.BASE_256B];
   buffer->db_depth_base = db_depth_base;

   if (!buffer->object) {
      auto format = db_depth_info.FORMAT;
      auto pitch_tile_max = db_depth_size.PITCH_TILE_MAX;
      auto slice_tile_max = db_depth_size.SLICE_TILE_MAX;

      auto pitch = gsl::narrow_cast<gl::GLsizei>((pitch_tile_max + 1) * latte::tile_width);
      auto height = gsl::narrow_cast<gl::GLsizei>(((slice_tile_max + 1) * (latte::tile_width * latte::tile_height)) / pitch);

      // Create depth buffer
      gl::glGenTextures(1, &buffer->object);
      gl::glBindTexture(gl::GL_TEXTURE_2D, buffer->object);
      gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAG_FILTER, static_cast<int>(gl::GL_NEAREST));
      gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MIN_FILTER, static_cast<int>(gl::GL_NEAREST));
      gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_WRAP_S, static_cast<int>(gl::GL_CLAMP_TO_EDGE));
      gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_WRAP_T, static_cast<int>(gl::GL_CLAMP_TO_EDGE));
      gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_COMPARE_MODE, static_cast<int>(gl::GL_NONE));
      gl::glTexImage2D(gl::GL_TEXTURE_2D, 0, static_cast<int>(gl::GL_DEPTH_COMPONENT32), pitch, height, 0, gl::GL_DEPTH_COMPONENT, gl::GL_FLOAT, 0);
   }

   return buffer;
}

bool GLDriver::checkActiveColorBuffer()
{
   for (auto i = 0u; i < 8; ++i) {
      auto cb_color_base = getRegister<latte::CB_COLORN_BASE>(latte::Register::CB_COLOR0_BASE + i * 4);
      auto &active = mActiveColorBuffers[i];

      if (!cb_color_base.BASE_256B) {
         if (active) {
            // Unbind active
            gl::glFramebufferTexture(gl::GL_FRAMEBUFFER, gl::GL_COLOR_ATTACHMENT0 + i, 0, 0);
            active = nullptr;
         }

         continue;
      }

      if (active && active->cb_color_base.BASE_256B == cb_color_base.BASE_256B) {
         // Already bound
         continue;
      }

      auto cb_color_size = getRegister<latte::CB_COLORN_SIZE>(latte::Register::CB_COLOR0_SIZE + i * 4);
      auto cb_color_info = getRegister<latte::CB_COLORN_INFO>(latte::Register::CB_COLOR0_INFO + i * 4);
      active = getColorBuffer(cb_color_base, cb_color_size, cb_color_info);

      // Bind color buffer
      gl::glFramebufferTexture(gl::GL_FRAMEBUFFER, gl::GL_COLOR_ATTACHMENT0 + i, active->object, 0);
   }

   return true;
}

bool GLDriver::checkActiveDepthBuffer()
{
   auto db_depth_base = getRegister<latte::DB_DEPTH_BASE>(latte::Register::DB_DEPTH_BASE);
   auto &active = mActiveDepthBuffer;

   if (!db_depth_base.BASE_256B) {
      if (active) {
         // Unbind active
         gl::glFramebufferTexture(gl::GL_FRAMEBUFFER, gl::GL_DEPTH_ATTACHMENT, 0, 0);
         active = nullptr;
      }

      return true;
   }

   if (active && active->db_depth_base.BASE_256B == db_depth_base.BASE_256B) {
      // Already bound
      return true;
   }

   auto db_depth_size = getRegister<latte::DB_DEPTH_SIZE>(latte::Register::DB_DEPTH_SIZE);
   auto db_depth_info = getRegister<latte::DB_DEPTH_INFO>(latte::Register::DB_DEPTH_INFO);
   active = getDepthBuffer(db_depth_base, db_depth_size, db_depth_info);

   // Bind depth buffer
   gl::glFramebufferTexture(gl::GL_FRAMEBUFFER, gl::GL_DEPTH_ATTACHMENT, active->object, 0);
   return true;
}

bool GLDriver::checkActiveUniforms()
{
   auto sq_config = getRegister<latte::SQ_CONFIG>(latte::Register::SQ_CONFIG);

   if (!mActiveShader) {
      return true;
   }

   if (sq_config.DX9_CONSTS) {
      // Upload uniform registers
      if (mActiveShader->vertex && mActiveShader->vertex->object) {
         auto values = reinterpret_cast<float *>(&mRegisters[latte::Register::SQ_ALU_CONSTANT0_256 / 4]);
         gl::glProgramUniform4fv(mActiveShader->vertex->object, mActiveShader->vertex->uniformRegisters, 256 * 4, values);
      }

      if (mActiveShader->pixel && mActiveShader->pixel->object) {
         auto values = reinterpret_cast<float *>(&mRegisters[latte::Register::SQ_ALU_CONSTANT0_0 / 4]);
         gl::glProgramUniform4fv(mActiveShader->pixel->object, mActiveShader->pixel->uniformRegisters, 256 * 4, values);
      }
   } else {
      // TODO: Upload uniform blocks
   }

   return true;
}

bool GLDriver::checkActiveShader()
{
   auto pgm_start_fs = getRegister<latte::SQ_PGM_START_FS>(latte::Register::SQ_PGM_START_FS);
   auto pgm_start_vs = getRegister<latte::SQ_PGM_START_VS>(latte::Register::SQ_PGM_START_VS);
   auto pgm_start_ps = getRegister<latte::SQ_PGM_START_PS>(latte::Register::SQ_PGM_START_PS);
   auto pgm_size_fs = getRegister<latte::SQ_PGM_SIZE_FS>(latte::Register::SQ_PGM_SIZE_FS);
   auto pgm_size_vs = getRegister<latte::SQ_PGM_SIZE_VS>(latte::Register::SQ_PGM_SIZE_VS);
   auto pgm_size_ps = getRegister<latte::SQ_PGM_SIZE_PS>(latte::Register::SQ_PGM_SIZE_PS);

   if (mActiveShader &&
       mActiveShader->fetch && mActiveShader->fetch->pgm_start_fs.PGM_START != pgm_start_fs.PGM_START
       && mActiveShader->vertex && mActiveShader->vertex->pgm_start_vs.PGM_START != pgm_start_vs.PGM_START
       && mActiveShader->pixel && mActiveShader->pixel->pgm_start_ps.PGM_START != pgm_start_ps.PGM_START) {
      // OpenGL shader matches latte shader
      return true;
   }

   // Update OpenGL shader
   auto &fetchShader = mFetchShaders[pgm_start_fs.PGM_START];
   auto &vertexShader = mVertexShaders[pgm_start_vs.PGM_START];
   auto &pixelShader = mPixelShaders[pgm_start_ps.PGM_START];
   auto &shader = mShaders[ShaderKey { pgm_start_fs.PGM_START, pgm_start_vs.PGM_START, pgm_start_ps.PGM_START }];

   auto getProgramLog = [](auto program, auto getFn, auto getInfoFn) {
      gl::GLint logLength = 0;
      std::string logMessage;
      getFn(program, gl::GL_INFO_LOG_LENGTH, &logLength);

      logMessage.resize(logLength);
      getInfoFn(program, logLength, &logLength, &logMessage[0]);
      return logMessage;
   };

   // Genearte shader if needed
   if (!shader.object) {
      // Parse fetch shader if needed
      if (!fetchShader.parsed) {
         auto program = make_virtual_ptr<void>(pgm_start_fs.PGM_START << 8);
         auto size = pgm_size_fs.PGM_SIZE << 3;

         if (!parseFetchShader(fetchShader, program, size)) {
            gLog->error("Failed to parse fetch shader");
            return false;
         }
      }

      // Compile vertex shader if needed
      if (!vertexShader.object) {
         auto program = make_virtual_ptr<uint8_t>(pgm_start_vs.PGM_START << 8);
         auto size = pgm_size_vs.PGM_SIZE << 3;

         if (!compileVertexShader(vertexShader, fetchShader, program, size)) {
            gLog->error("Failed to recompile vertex shader");
            return false;
         }

         // Create OpenGL Shader
         const gl::GLchar *code[] = { vertexShader.code.c_str() };
         vertexShader.object = gl::glCreateShaderProgramv(gl::GL_VERTEX_SHADER, 1, code);

         // Check program log
         auto log = getProgramLog(vertexShader.object, gl::glGetProgramiv, gl::glGetProgramInfoLog);
         if (log.size()) {
            gLog->error("OpenGL failed to compile vertex shader:\n{}", log);
            gLog->error("Shader Code:\n{}\n", vertexShader.code);
            return false;
         }

         // Get uniform locations
         vertexShader.uniformRegisters = gl::glGetUniformLocation(vertexShader.object, "VC");
      }

      // Compile pixel shader if needed
      if (!pixelShader.object) {
         auto program = make_virtual_ptr<uint8_t>(pgm_start_ps.PGM_START << 8);
         auto size = pgm_size_ps.PGM_SIZE << 3;

         if (!compilePixelShader(pixelShader, program, size)) {
            gLog->error("Failed to recompile pixel shader");
            return false;
         }

         // Create OpenGL Shader
         const gl::GLchar *code[] = { pixelShader.code.c_str() };
         pixelShader.object = gl::glCreateShaderProgramv(gl::GL_FRAGMENT_SHADER, 1, code);

         // Check program log
         auto log = getProgramLog(pixelShader.object, gl::glGetProgramiv, gl::glGetProgramInfoLog);
         if (log.size()) {
            gLog->error("OpenGL failed to compile pixel shader:\n{}", log);
            gLog->error("Shader Code:\n{}\n", pixelShader.code);
            return false;
         }

         // Get uniform locations
         pixelShader.uniformRegisters = gl::glGetUniformLocation(pixelShader.object, "VC");
      }

      if (fetchShader.parsed && vertexShader.object && pixelShader.object) {
         shader.fetch = &fetchShader;
         shader.vertex = &vertexShader;
         shader.pixel = &pixelShader;

         // Create pipeline
         gl::glGenProgramPipelines(1, &shader.object);
         gl::glUseProgramStages(shader.object, gl::GL_VERTEX_SHADER_BIT, shader.vertex->object);
         gl::glUseProgramStages(shader.object, gl::GL_FRAGMENT_SHADER_BIT, shader.pixel->object);
      }
   }

   // Bind shader
   gl::glBindProgramPipeline(shader.object);
   return true;
}

void GLDriver::setRegister(latte::Register::Value reg, uint32_t value)
{
   // Save to my state
   mRegisters[reg / 4] = value;

   // TODO: Save to active context state shadow regs

   // For the following registers, we apply their state changes
   //   directly to the OpenGL context...
   switch (reg) {
   case latte::Register::SQ_VTX_SEMANTIC_CLEAR:
      for (auto i = 0u; i < 32; ++i) {
         setRegister(static_cast<latte::Register::Value>(latte::Register::SQ_VTX_SEMANTIC_0 + i * 4), 0xffffffff);
      }
      break;
   case latte::Register::CB_BLEND_CONTROL:
   case latte::Register::CB_BLEND0_CONTROL:
   case latte::Register::CB_BLEND1_CONTROL:
   case latte::Register::CB_BLEND2_CONTROL:
   case latte::Register::CB_BLEND3_CONTROL:
   case latte::Register::CB_BLEND4_CONTROL:
   case latte::Register::CB_BLEND5_CONTROL:
   case latte::Register::CB_BLEND6_CONTROL:
   case latte::Register::CB_BLEND7_CONTROL:
      // gl::something();
      break;
   }
}

static std::string
readFileToString(const std::string &filename)
{
   std::ifstream in { filename, std::ifstream::binary };
   std::string result;

   if (in.is_open()) {
      in.seekg(0, in.end);
      auto size = in.tellg();
      result.resize(size);
      in.seekg(0, in.beg);
      in.read(&result[0], size);
   }

   return result;
}

void GLDriver::initGL()
{
   activateDeviceContext();
   glbinding::Binding::initialize();

   glbinding::setCallbackMaskExcept(glbinding::CallbackMask::After, { "glGetError" });
   glbinding::setAfterCallback([](const glbinding::FunctionCall &call) {
      auto error = gl::glGetError();

      if (error != gl::GL_NO_ERROR) {
         gLog->error("OpenGL {} error: {}", call.toString(), error);
      }
   });

   // Clear active state
   mActiveShader = nullptr;
   mActiveDepthBuffer = nullptr;
   memset(&mActiveColorBuffers[0], 0, sizeof(ColorBuffer *) * mActiveColorBuffers.size());

   // Create our default framebuffer
   gl::glGenFramebuffers(1, &mFrameBuffer.object);
   gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, mFrameBuffer.object);

   auto vertexCode = readFileToString("resources/shaders/screen_vertex.glsl");
   if (!vertexCode.size()) {
      gLog->error("Could not load resources/shaders/screen_vertex.glsl");
   }

   auto pixelCode = readFileToString("resources/shaders/screen_pixel.glsl");
   if (!pixelCode.size()) {
      gLog->error("Could not load resources/shaders/screen_pixel.glsl");
   }

   // Create vertex program
   auto code = vertexCode.c_str();
   mScreenDraw.vertexProgram = gl::glCreateShaderProgramv(gl::GL_VERTEX_SHADER, 1, &code);

   // Create pixel program
   code = pixelCode.c_str();
   mScreenDraw.pixelProgram = gl::glCreateShaderProgramv(gl::GL_FRAGMENT_SHADER, 1, &code);
   gl::glBindFragDataLocation(mScreenDraw.pixelProgram, 0, "ps_color");

   gl::GLint logLength = 0;
   std::string logMessage;
   gl::glGetProgramiv(mScreenDraw.vertexProgram, gl::GL_INFO_LOG_LENGTH, &logLength);

   logMessage.resize(logLength);
   gl::glGetProgramInfoLog(mScreenDraw.vertexProgram, logLength, &logLength, &logMessage[0]);
   gLog->error("Failed to compile vertex shader glsl:\n{}", logMessage);

   // Create pipeline
   gl::glGenProgramPipelines(1, &mScreenDraw.pipeline);
   gl::glBindProgramPipeline(mScreenDraw.pipeline);
   gl::glUseProgramStages(mScreenDraw.pipeline, gl::GL_VERTEX_SHADER_BIT, mScreenDraw.vertexProgram);
   gl::glUseProgramStages(mScreenDraw.pipeline, gl::GL_FRAGMENT_SHADER_BIT, mScreenDraw.pixelProgram);

   // Create vertex buffer
   static const gl::GLfloat vertices[] = {
       -1.0f,  1.0f,  0.0f, 1.0f,
        1.0f,  1.0f,  1.0f, 1.0f,
        1.0f, -1.0f,  1.0f, 0.0f,

        1.0f, -1.0f,  1.0f, 0.0f,
       -1.0f, -1.0f,  0.0f, 0.0f,
       -1.0f,  1.0f,  0.0f, 1.0f
   };

   gl::glGenBuffers(1, &mScreenDraw.vertBuffer);
   gl::glBindBuffer(gl::GL_ARRAY_BUFFER, mScreenDraw.vertBuffer);
   gl::glBufferData(gl::GL_ARRAY_BUFFER, sizeof(vertices), vertices, gl::GL_STATIC_DRAW);

   // Create vertex array
   gl::glGenVertexArrays(1, &mScreenDraw.vertArray);
   gl::glBindVertexArray(mScreenDraw.vertArray);
   gl::glBindBuffer(gl::GL_ARRAY_BUFFER, mScreenDraw.vertBuffer);

   auto fs_position = gl::glGetAttribLocation(mScreenDraw.vertexProgram, "fs_position");
   gl::glEnableVertexAttribArray(fs_position);
   gl::glVertexAttribPointer(fs_position, 2, gl::GL_FLOAT, gl::GL_FALSE, 4 * sizeof(gl::GLfloat), 0);

   auto fs_texCoord = gl::glGetAttribLocation(mScreenDraw.vertexProgram, "fs_texCoord");
   gl::glEnableVertexAttribArray(fs_texCoord);
   gl::glVertexAttribPointer(fs_texCoord, 2, gl::GL_FLOAT, gl::GL_FALSE, 4 * sizeof(gl::GLfloat), (void*)(2 * sizeof(gl::GLfloat)));
}

void GLDriver::decafCopyColorToScan(pm4::DecafCopyColorToScan &data)
{
   auto cb_color_base = bit_cast<latte::CB_COLORN_BASE>(data.bufferAddr);
   auto buffer = getColorBuffer(cb_color_base, data.cb_color_size, data.cb_color_info);

   if (data.scanTarget == 1) {
      // TV
   } else if (data.scanTarget == 4) {
      // DRC
   }

   // Unbind active framebuffer
   gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, 0);

   // Setup screen draw shader
   gl::glBindVertexArray(mScreenDraw.vertArray);
   gl::glBindProgramPipeline(mScreenDraw.pipeline);

   // Draw screen quad
   gl::glEnable(gl::GL_TEXTURE_2D);
   gl::glDisable(gl::GL_DEPTH_TEST);
   gl::glActiveTexture(gl::GL_TEXTURE0);
   gl::glBindTexture(gl::GL_TEXTURE_2D, buffer->object);
   gl::glDrawArrays(gl::GL_TRIANGLES, 0, 6);

   // Rebind active framebuffer
   gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, mFrameBuffer.object);
}

void GLDriver::decafSwapBuffers(pm4::DecafSwapBuffers &data)
{
   swapBuffers();
}

void GLDriver::decafClearColor(pm4::DecafClearColor &data)
{
   float colors[] = {
      data.red,
      data.green,
      data.blue,
      data.alpha
   };

   // Check if the color buffer is actively bound
   for (auto i = 0; i < 8; ++i) {
      auto active = mActiveColorBuffers[i];

      if (!active) {
         continue;
      }

      if (active->cb_color_base.BASE_256B == data.bufferAddr) {
         gl::glClearBufferfv(gl::GL_COLOR, i, colors);
         return;
      }
   }

   // Find our colorbuffer to clear
   auto cb_color_base = bit_cast<latte::CB_COLORN_BASE>(data.bufferAddr);
   auto buffer = getColorBuffer(cb_color_base, data.cb_color_size, data.cb_color_info);

   // Temporarily set to this color buffer
   gl::glFramebufferTexture(gl::GL_FRAMEBUFFER, gl::GL_COLOR_ATTACHMENT0, buffer->object, 0);

   // Clear the buffer
   gl::glClearBufferfv(gl::GL_COLOR, 0, colors);

   // Clear the temporary color buffer attachement
   mActiveColorBuffers[0] = nullptr;
   gl::glFramebufferTexture(gl::GL_FRAMEBUFFER, gl::GL_COLOR_ATTACHMENT0, 0, 0);
}

void GLDriver::decafClearDepthStencil(pm4::DecafClearDepthStencil &data)
{
}

void GLDriver::drawIndexAuto(pm4::DrawIndexAuto &data)
{
   if (!checkReadyDraw()) {
      return;
   }
}

void GLDriver::drawIndex2(pm4::DrawIndex2 &data)
{
   if (!checkReadyDraw()) {
      return;
   }
}

void GLDriver::indexType(pm4::IndexType &data)
{
   mRegisters[latte::Register::VGT_DMA_INDEX_TYPE / 4] = data.type.value;
}

void GLDriver::numInstances(pm4::NumInstances &data)
{
   mRegisters[latte::Register::VGT_DMA_NUM_INSTANCES / 4] = data.count;
}

void GLDriver::setAluConsts(pm4::SetAluConsts &data)
{
   for (auto i = 0u; i < data.values.size(); ++i) {
      setRegister(static_cast<latte::Register::Value>(data.id + i * 4), data.values[i]);
   }
}

void GLDriver::setConfigRegs(pm4::SetConfigRegs &data)
{
   for (auto i = 0u; i < data.values.size(); ++i) {
      setRegister(static_cast<latte::Register::Value>(data.id + i * 4), data.values[i]);
   }
}

void GLDriver::setContextRegs(pm4::SetContextRegs &data)
{
   for (auto i = 0u; i < data.values.size(); ++i) {
      setRegister(static_cast<latte::Register::Value>(data.id + i * 4), data.values[i]);
   }
}

void GLDriver::setControlConstants(pm4::SetControlConstants &data)
{
   for (auto i = 0u; i < data.values.size(); ++i) {
      setRegister(static_cast<latte::Register::Value>(data.id + i * 4), data.values[i]);
   }
}

void GLDriver::setLoopConsts(pm4::SetLoopConsts &data)
{
   for (auto i = 0u; i < data.values.size(); ++i) {
      setRegister(static_cast<latte::Register::Value>(data.id + i * 4), data.values[i]);
   }
}

void GLDriver::setSamplers(pm4::SetSamplers &data)
{
   for (auto i = 0u; i < data.values.size(); ++i) {
      setRegister(static_cast<latte::Register::Value>(data.id + i * 4), data.values[i]);
   }
}

void GLDriver::setResources(pm4::SetResources &data)
{
   for (auto i = 0u; i < data.values.size(); ++i) {
      setRegister(static_cast<latte::Register::Value>(data.id + i * 4), data.values[i]);
   }
}

void GLDriver::indirectBufferCall(pm4::IndirectBufferCall &data)
{
   auto buffer = reinterpret_cast<uint32_t*>(data.addr.get());
   runCommandBuffer(buffer, data.size);
}

void GLDriver::handlePacketType3(pm4::Packet3 header, gsl::array_view<uint32_t> data)
{
   pm4::PacketReader reader { data };

   switch (header.opcode) {
   case pm4::Opcode3::DECAF_COPY_COLOR_TO_SCAN:
      decafCopyColorToScan(pm4::read<pm4::DecafCopyColorToScan>(reader));
      break;
   case pm4::Opcode3::DECAF_SWAP_BUFFERS:
      decafSwapBuffers(pm4::read<pm4::DecafSwapBuffers>(reader));
      break;
   case pm4::Opcode3::DECAF_CLEAR_COLOR:
      decafClearColor(pm4::read<pm4::DecafClearColor>(reader));
      break;
   case pm4::Opcode3::DECAF_CLEAR_DEPTH_STENCIL:
      decafClearDepthStencil(pm4::read<pm4::DecafClearDepthStencil>(reader));
      break;
   case pm4::Opcode3::DRAW_INDEX_AUTO:
      drawIndexAuto(pm4::read<pm4::DrawIndexAuto>(reader));
      break;
   case pm4::Opcode3::DRAW_INDEX_2:
      drawIndex2(pm4::read<pm4::DrawIndex2>(reader));
      break;
   case pm4::Opcode3::INDEX_TYPE:
      indexType(pm4::read<pm4::IndexType>(reader));
      break;
   case pm4::Opcode3::NUM_INSTANCES:
      numInstances(pm4::read<pm4::NumInstances>(reader));
      break;
   case pm4::Opcode3::SET_ALU_CONST:
      setAluConsts(pm4::read<pm4::SetAluConsts>(reader));
      break;
   case pm4::Opcode3::SET_CONFIG_REG:
      setConfigRegs(pm4::read<pm4::SetConfigRegs>(reader));
      break;
   case pm4::Opcode3::SET_CONTEXT_REG:
      setContextRegs(pm4::read<pm4::SetContextRegs>(reader));
      break;
   case pm4::Opcode3::SET_CTL_CONST:
      setControlConstants(pm4::read<pm4::SetControlConstants>(reader));
      break;
   case pm4::Opcode3::SET_LOOP_CONST:
      setLoopConsts(pm4::read<pm4::SetLoopConsts>(reader));
      break;
   case pm4::Opcode3::SET_SAMPLER:
      setSamplers(pm4::read<pm4::SetSamplers>(reader));
      break;
   case pm4::Opcode3::SET_RESOURCE:
      setResources(pm4::read<pm4::SetResources>(reader));
      break;
   case pm4::Opcode3::INDIRECT_BUFFER_PRIV:
      indirectBufferCall(pm4::read<pm4::IndirectBufferCall>(reader));
      break;
   }
}

void GLDriver::start()
{
   mRunning = true;
   mThread = std::thread(&GLDriver::run, this);
}

void GLDriver::setTvDisplay(size_t width, size_t height)
{
}

void GLDriver::setDrcDisplay(size_t width, size_t height)
{
}

void GLDriver::runCommandBuffer(uint32_t *buffer, uint32_t size)
{
   for (auto pos = 0u; pos < size; ) {
      auto header = *reinterpret_cast<pm4::PacketHeader *>(&buffer[pos]);
      auto size = 0u;

      switch (header.type) {
      case pm4::PacketType::Type3:
      {
         auto header3 = pm4::Packet3{ header.value };
         size = header3.size + 1;
         handlePacketType3(header3, { &buffer[pos + 1], size });
         break;
      }
      case pm4::PacketType::Type0:
      case pm4::PacketType::Type1:
      case pm4::PacketType::Type2:
      default:
         throw std::logic_error("What the fuck son");
      }

      pos += size + 1;
   }
}

void GLDriver::run()
{
   initGL();

   while (mRunning) {
      auto buffer = gpu::unqueueCommandBuffer();
      runCommandBuffer(buffer->buffer, buffer->curSize);
      gpu::retireCommandBuffer(buffer);
   }
}

} // namespace opengl

} // namespace gpu