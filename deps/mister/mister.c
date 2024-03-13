#include "mister.h"
#include <gfx/video_frame.h>
#include "../gfx/common/gl2_common.h"
#include "../gfx/common/gl3_defines.h"
#include "../gfx/common/vulkan_common.h"

#define GL_VIEWPORT_HACK 1
#define MAX_BUFFER_WIDTH 1024
#define MAX_BUFFER_HEIGHT 768

static mister_video_t mister_video;
static sr_mode mister_mode;
static char *menu_buffer = 0;
static unsigned menu_width = 0;
static unsigned menu_height = 0;
static bool modeline_active = 0;
static bool mode_switch_pending = 0;
static bool vp_resize_pending = 0;
static bool prev_menu_state = 0;
struct scaler_ctx *scaler;

uint8_t *mister_buffer = 0;
uint8_t *scaled_buffer = 0;
uint8_t *hardware_buffer = 0;

union
{
   const uint8_t *u8;
   const uint16_t *u16;
   const uint32_t *u32;
} u;

void mister_resize_viewport(video_driver_state_t *video_st);

void mister_set_menu_buffer(char *frame, unsigned width, unsigned height)
{
   menu_buffer = frame;
   menu_width = width;
   menu_height = height;
}

void mister_draw(video_driver_state_t *video_st, const void *data, unsigned width, unsigned height, size_t pitch)
{
   settings_t *settings  = config_get_ptr();
   audio_driver_state_t *audio_st  = audio_state_get_ptr();
   uint8_t field = 0;
   uint8_t format = 0;
   bool menu_on = false;
   bool stretched = false;
   bool must_clear_buffer = false;
   bool is_hw_rendered = (data == RETRO_HW_FRAME_BUFFER_VALID
                           && video_st->frame_cache_data == RETRO_HW_FRAME_BUFFER_VALID);


   // Initialize MiSTer if required
   if (!mister_video.isConnected)
      mister_CmdInit(settings->arrays.mister_ip, 32100, settings->bools.mister_lz4, settings->uints.audio_output_sample_rate, 2);

   // Send audio first
   if (audio_st->output_mister && audio_st->output_mister_samples)
   {
      mister_CmdAudio(audio_st->output_mister_samples_conv_buf, audio_st->output_mister_samples >> 1, 2);
      audio_st->output_mister_samples = 0;
   }

   // Check if we need to mode switch
   if (mode_switch_pending)
   {
      mister_CmdSwitchres(&mister_mode);

      video_driver_set_size(mister_mode.width, mister_mode.height);
      vp_resize_pending = true;
      must_clear_buffer = true;
   }

   if (!modeline_active)
      return;

   retro_time_t mister_bt1  = cpu_features_get_time_usec();

   // Get pixel format
   if (video_st->pix_fmt == RETRO_PIXEL_FORMAT_XRGB8888)
      format = SCALER_FMT_ARGB8888;

   else if (video_st->pix_fmt == RETRO_PIXEL_FORMAT_RGB565)
      format = SCALER_FMT_RGB565;

   else
      // Unsupported pixel format
      return;

   // Get menu bitmap dimensions
   #ifdef HAVE_MENU
   if (menu_state_get_ptr()->flags & MENU_ST_FLAG_ALIVE && menu_buffer != 0)
   {
      menu_on = true;
      width = menu_width;
      height = menu_height;
      pitch = width * sizeof(uint16_t);
      format = SCALER_FMT_RGBA4444;
      data = menu_buffer;
   }
   if (prev_menu_state != menu_on)
   {
      prev_menu_state = menu_on;
      must_clear_buffer = true;
   }
   #endif

   // Get RGB buffer if hw rendered
   if (is_hw_rendered)
   {
      if (vp_resize_pending)
      {
         mister_resize_viewport(video_st);
         vp_resize_pending = false;
      }

      if (video_st->current_video->read_viewport
            && video_st->current_video->read_viewport(video_st->data, hardware_buffer, false))
         pitch = mister_video.width * 3;

      else return;

      format = SCALER_FMT_BGR24;
      data = hardware_buffer;
   }

   if (pitch == 0)
      return;

   // Scale frame
   double x_scale, y_scale;
   if (menu_on)
   {
      x_scale = (double)mister_mode.width / (double)width;
      y_scale = (double)mister_mode.height / (double)height;
      stretched = x_scale != floor(x_scale) || y_scale != floor(y_scale);
   }
   else
   {
      x_scale = (retroarch_get_rotation() & 1) ? mister_mode.y_scale : mister_mode.x_scale;
      y_scale = (retroarch_get_rotation() & 1) ? mister_mode.x_scale : mister_mode.y_scale;
      stretched = mister_mode.is_stretched;
   }

   if (x_scale != 1.0 || y_scale != 1.0)
   {
      u_int scaler_width  = round(width * x_scale);
      u_int scaler_height = round(height * y_scale);
      u_int scaler_pitch  = scaler_width * sizeof(uint32_t);

      if (  width  != (u_int)scaler->in_width
         || height != (u_int)scaler->in_height
         || format != scaler->in_fmt
         || pitch  != (u_int)scaler->in_stride
         || scaler_width  != (u_int)scaler->out_width
         || scaler_height != (u_int)scaler->out_height)
      {
         scaler->scaler_type = stretched ? SCALER_TYPE_BILINEAR : SCALER_TYPE_POINT;
         scaler->in_fmt    = format;
         scaler->in_width  = width;
         scaler->in_height = height;
         scaler->in_stride = pitch;

         scaler->out_width  = scaler_width;
         scaler->out_height = scaler_height;
         scaler->out_stride = scaler_pitch;

         scaler_ctx_gen_filter(scaler);
      }

      scaler_ctx_scale_direct(scaler, scaled_buffer, data);

      width  = scaler->out_width;
      height = scaler->out_height;
      pitch  = scaler->out_stride;
      format = scaler->out_fmt;
      data = scaled_buffer;
   }

   // Clear frame buffer if required
   if (must_clear_buffer)
   {
      must_clear_buffer = false;
      memset(mister_buffer, 0, MAX_BUFFER_WIDTH * MAX_BUFFER_HEIGHT * 3);
   }

   // Compute borders and clipping
   u_int rotation = retroarch_get_rotation();
   u_int rot_width = (rotation & 1) && !menu_on ? height : width;
   u_int rot_height = (rotation & 1) && !menu_on ?  width : height;

   u_int x_start = mister_video.width > rot_width ? (mister_video.width - rot_width) / 2 : 0;
   u_int x_crop = mister_video.width < rot_width ? rot_width - mister_video.width : 0;
   u_int x_max = rot_width - x_crop;
   u_int y_start = mister_video.height > rot_height ? (mister_video.height - rot_height) / 2 : 0;
   u_int y_crop = mister_video.height < rot_height ? (rot_height - mister_video.height) : 0;
   u_int y_max = rot_height - y_crop;

   if (mister_isInterlaced())
   {
      y_start /= 2;
      y_crop /= 2;

      if (!(rotation & 1) || menu_on)
         y_max /= 2;
   }

   if ((rotation & 1) && !menu_on)
   {
      u_int tmp;
      tmp = x_max;
      x_max = y_max;
      y_max = tmp;
   }

   // Get first pixel address from our RGB source
   field = mister_GetField();
   u.u8 = data;
   u.u8 += (field + (y_crop / 2)) * pitch + (x_crop / 2);

   // Compute steps to walk through the source & target bitmaps
   u_int c = 0;
   int c_step = 3;
   int s_step = 1;
   int r_step = 1;

   if (menu_on || is_hw_rendered)
      r_step = mister_isInterlaced() ? 2 : 1;

   else switch (rotation)
   {
      case ORIENTATION_NORMAL:
      case ORIENTATION_FLIPPED:
         c_step = 3;
         r_step = mister_isInterlaced() ? 2 : 1;
         break;

      case ORIENTATION_VERTICAL:
         c_step = -mister_video.width * 3;
         s_step = mister_isInterlaced() ? 2 : 1;
         break;

      case ORIENTATION_FLIPPED_ROTATED:
         c_step = +mister_video.width * 3;
         s_step = mister_isInterlaced() ? 2 : 1;
         break;
   }

   // Copy RGB buffer
   for (u_int j = 0; j < y_max - 1; j++)
   {
      if (is_hw_rendered)
         c = (mister_video.width * (mister_video.height / r_step - y_start - field - j) + x_start) * 3;

      else if (menu_on || !(rotation & 1))
         c = ((j + y_start) * mister_video.width + x_start) * 3;

      else if (rotation == ORIENTATION_VERTICAL)
         c = (mister_video.width * (mister_video.height / s_step - y_start - 1) + j + x_start) * 3;

      else if (rotation == ORIENTATION_FLIPPED_ROTATED)
         c = (mister_video.width * (y_start + 1) - j - x_start - 1) * 3;

      for (u_int i = 0; i < x_max - 1; i += s_step)
      {
         if (format == SCALER_FMT_RGBA4444)
         {
            uint16_t pixel = u.u16[i];
            mister_buffer[c + 0] = (pixel >>  8); //b
            mister_buffer[c + 1] = (pixel >>  4); //g
            mister_buffer[c + 2] = (pixel >>  0); //r
         }
         else if (format == SCALER_FMT_BGR24)
         {
            u_int pixel = i * 3;
            mister_buffer[c + 0] = u.u8[pixel + 0]; //b
            mister_buffer[c + 1] = u.u8[pixel + 1]; //g
            mister_buffer[c + 2] = u.u8[pixel + 2]; //r
         }
         else if (format == SCALER_FMT_RGB565)
         {
            uint16_t pixel = u.u16[i];
            uint8_t r  = (pixel >> 11) & 0x1f;
            uint8_t g  = (pixel >>  5) & 0x3f;
            uint8_t b  = (pixel >>  0) & 0x1f;
            mister_buffer[c + 0] = (b << 3) | (b >> 2); //b
            mister_buffer[c + 1] = (g << 2) | (g >> 4); //g
            mister_buffer[c + 2] = (r << 3) | (r >> 2); //r
         }
         else if (format == SCALER_FMT_ARGB8888)
         {
            uint32_t pixel = u.u32[i];
            mister_buffer[c + 0] = (pixel >>  0) & 0xff; //b
            mister_buffer[c + 1] = (pixel >>  8) & 0xff; //g
            mister_buffer[c + 2] = (pixel >> 16) & 0xff; //r
         }

         c += c_step;
      }

      u.u8 += pitch * r_step;
   }

   // Compute sync scanline based on frame delay
   int mister_vsync = 1;
   if (settings->bools.video_frame_delay_auto && video_st->frame_delay_effective > 0)
      mister_vsync = height / (16 / video_st->frame_delay_effective);

   else if (settings->uints.video_frame_delay > 0)
      mister_vsync = height / (16 / settings->uints.video_frame_delay);

   // Blit to MiSTer
   mister_CmdBlit((char *)mister_buffer, mister_vsync);
   retro_time_t mister_bt2  = cpu_features_get_time_usec();

   mister_setBlitTime(mister_bt2 - mister_bt1);
}

void mister_resize_viewport(video_driver_state_t *video_st)
{
#if GL_VIEWPORT_HACK
   if (string_is_equal(video_driver_get_ident(), "gl"))
   {
      gl2_t *gl = (gl2_t*)video_st->data;
      gl->video_width = mister_mode.width;
      gl->video_height = mister_mode.height;
      gl->vp.width = mister_mode.width;
      gl->vp.height = mister_mode.height;
      gl->pbo_readback_scaler.out_width = mister_mode.width;
      gl->pbo_readback_scaler.out_height = mister_mode.height;
      gl->pbo_readback_scaler.in_width = mister_mode.width;
      gl->pbo_readback_scaler.in_height = mister_mode.height;
      gl->pbo_readback_scaler.in_stride = mister_mode.width * sizeof(uint32_t);
      gl->pbo_readback_scaler.out_stride = mister_mode.width * 3;
   }
   else if (string_is_equal(video_driver_get_ident(), "glcore"))
   {
      gl3_t *gl = (gl3_t*)video_st->data;
      gl->video_width = mister_mode.width;
      gl->video_height = mister_mode.height;
      gl->vp.width = mister_mode.width;
      gl->vp.height = mister_mode.height;
      gl->pbo_readback_scaler.out_width = mister_mode.width;
      gl->pbo_readback_scaler.out_height = mister_mode.height;
      gl->pbo_readback_scaler.in_width = mister_mode.width;
      gl->pbo_readback_scaler.in_height = mister_mode.height;
      gl->pbo_readback_scaler.in_stride = mister_mode.width * sizeof(uint32_t);
      gl->pbo_readback_scaler.out_stride = mister_mode.width * 3;
   }
   else if (string_is_equal(video_driver_get_ident(), "vulkan"))
   {
      vk_t *vk = (vk_t*)video_st->data;
      vk->video_width = mister_mode.width;
      vk->video_height = mister_mode.height;
      vk->vp.width = mister_mode.width;
      vk->vp.height = mister_mode.height;
      vk->readback.scaler_bgr.in_width    = mister_mode.width;
      vk->readback.scaler_bgr.in_height   = mister_mode.height;
      vk->readback.scaler_bgr.out_width   = mister_mode.width;
      vk->readback.scaler_bgr.out_height  = mister_mode.height;
      vk->readback.scaler_bgr.in_stride   = mister_mode.width * sizeof(uint32_t);
      vk->readback.scaler_bgr.out_stride  = mister_mode.width * 3;
      vk->readback.scaler_bgr.in_fmt      = SCALER_FMT_ARGB8888;
      vk->readback.scaler_bgr.out_fmt     = SCALER_FMT_BGR24;
      vk->readback.scaler_bgr.scaler_type = SCALER_TYPE_POINT;

      vk->readback.scaler_rgb.in_width    = mister_mode.width;
      vk->readback.scaler_rgb.in_height   = mister_mode.height;
      vk->readback.scaler_rgb.out_width   = mister_mode.width;
      vk->readback.scaler_rgb.out_height  = mister_mode.height;
      vk->readback.scaler_rgb.in_stride   = mister_mode.width * sizeof(uint32_t);
      vk->readback.scaler_rgb.out_stride  = mister_mode.width * 3;
      vk->readback.scaler_rgb.in_fmt      = SCALER_FMT_ABGR8888;
      vk->readback.scaler_rgb.out_fmt     = SCALER_FMT_BGR24;
      vk->readback.scaler_rgb.scaler_type = SCALER_TYPE_POINT;
   }
#else
   settings_t *settings  = config_get_ptr();
   struct video_viewport vp = { 0 };
   video_driver_get_viewport_info(&vp);
   uint16_t mister_width = mister_GetWidth();
   uint16_t mister_height = mister_GetHeight();

   vp.x                = 0;
   vp.y                = 0;
   vp.width            = mister_width;
   vp.height           = mister_height;
   vp.full_width       = mister_width;
   vp.full_height      = mister_height;

   video_st->aspect_ratio = aspectratio_lut[ASPECT_RATIO_CUSTOM].value;
   settings->uints.video_aspect_ratio_idx = ASPECT_RATIO_CUSTOM;
   settings->video_viewport_custom = vp;
   video_driver_set_aspect_ratio();
   video_driver_update_viewport(&vp, false, true);
#endif
}



void mister_CmdClose(void)
{
   if (!mister_video.isConnected)
      return;

   RARCH_LOG("[MiSTer] Sending CMD_CLOSE...\n");

   char buffer[1];
   buffer[0] = mister_CMD_CLOSE;
   mister_Send((char*)&buffer[0], 1);

#ifdef WIN32
   closesocket(mister_video.sockfd);
   WSACleanup();
#else
   close(mister_video.sockfd);
#endif
   mister_video.isConnected = 0;
   modeline_active = 0;

   free(mister_buffer);
   free(scaled_buffer);
   free(hardware_buffer);
   mister_buffer = 0;
   scaled_buffer = 0;
   hardware_buffer = 0;

   scaler_ctx_gen_reset(scaler);
   free(scaler);
   scaler = 0;
}

void mister_CmdInit(const char* mister_host, short mister_port, bool lz4_frames,uint32_t sound_rate, uint8_t sound_chan)
{
   char buffer[4];

#ifdef _WIN32

   WSADATA wsd;
   uint16_t rc;

   RARCH_LOG("[MiSTer] Initialising Winsock...\n");
   // Load Winsock
   rc = WSAStartup(MAKEWORD(2, 2), &wsd);
   if (rc != 0)
   {
      RARCH_WARN("Unable to load Winsock: %d\n", rc);
   }
   RARCH_LOG("[MiSTer] Initialising socket...\n");
   mister_video.sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
   if (mister_video.sockfd < 0)
   {
      RARCH_WARN("socket error\n");
   }

   memset(&mister_video.ServerAddr, 0, sizeof(mister_video.ServerAddr));
   mister_video.ServerAddr.sin_family = AF_INET;
   mister_video.ServerAddr.sin_port = htons(mister_port);
   mister_video.ServerAddr.sin_addr.s_addr = inet_addr(mister_host);

   RARCH_LOG("[MiSTer] Setting socket async...\n");
   u_long iMode=1;
   rc = ioctlsocket(mister_video.sockfd, FIONBIO, &iMode);
   if (rc < 0)
   {
      RARCH_WARN("set nonblock fail\n");
   }

   RARCH_LOG("[MiSTer] Setting send buffer to 2097152 bytes...\n");
   int optVal = 2097152;
   int optLen = sizeof(int);
   rc = setsockopt(mister_video.sockfd, SOL_SOCKET, SO_SNDBUF, (char*)&optVal, optLen);
   if (rc < 0)
   {
      RARCH_WARN("set so_sndbuff fail\n");
   }

#else

   RARCH_LOG("[MiSTer] Initialising socket...\n");
   mister_video.sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
   if (mister_video.sockfd < 0)
   {
      RARCH_WARN("socket error\n");
   }

   memset(&mister_video.ServerAddr, 0, sizeof(mister_video.ServerAddr));
   mister_video.ServerAddr.sin_family = AF_INET;
   mister_video.ServerAddr.sin_port = htons(mister_port);
   mister_video.ServerAddr.sin_addr.s_addr = inet_addr(mister_host);

   RARCH_LOG("[MiSTer] Setting socket async...\n");
   // Non blocking socket
   int flags;
   flags = fcntl(mister_video.sockfd, F_GETFD, 0);
   if (flags < 0)
   {
      RARCH_WARN("get falg error\n");
   }
   flags |= O_NONBLOCK;
   if (fcntl(mister_video.sockfd, F_SETFL, flags) < 0)
   {
      RARCH_WARN("set nonblock fail\n");
   }

   RARCH_LOG("[MiSTer] Setting send buffer to 2097152 bytes...\n");
   // Settings
   int size = 2 * 1024 * 1024;
   if (setsockopt(mister_video.sockfd, SOL_SOCKET, SO_SNDBUF, (void*)&size, sizeof(size)) < 0)
   {
      RARCH_WARN("Error so_sndbuff\n");
   }

#endif

   RARCH_LOG("[MiSTer] Sending CMD_INIT...lz4 %d sound_rate %d sound_chan %d\n", lz4_frames, sound_rate, sound_chan);

   buffer[0] = mister_CMD_INIT;
   buffer[1] = (lz4_frames) ? 1 : 0; //0-RAW or 1-LZ4 ;
   buffer[2] = (sound_rate == 22050) ? 1 : (sound_rate == 44100) ? 2 : (sound_rate == 48000) ? 3 : 0;
   buffer[3] = sound_chan;


   mister_Send(&buffer[0], 4);

   mister_video.lz4_compress = lz4_frames;
   mister_video.width = 0;
   mister_video.height = 0;
   mister_video.width_core = 0;
   mister_video.height_core = 0;
   mister_video.lines = 0;
   mister_video.lines_padding = 0;
   mister_video.vfreq = 0;
   mister_video.vfreq_core = 0;
   mister_video.widthTime = 0;
   mister_video.frameTime = 0;
   mister_video.avgEmulationTime = 0;
   mister_video.vsync_auto = 120;
   mister_video.avgBlitTime = 0;
   mister_video.frameEcho = 0;
   mister_video.vcountEcho = 0;
   mister_video.frameGPU = 0;
   mister_video.vcountGPU = 0;
   mister_video.interlaced = 0;
   mister_video.fpga_audio = 0;
   mister_video.isConnected = true;

   // Allocate buffers
    mister_buffer = (uint8_t*)malloc(MAX_BUFFER_WIDTH * MAX_BUFFER_HEIGHT * 3);
    scaled_buffer = (uint8_t*)malloc(MAX_BUFFER_WIDTH * MAX_BUFFER_HEIGHT * 4);
    hardware_buffer = (uint8_t*)malloc(MAX_BUFFER_WIDTH * MAX_BUFFER_HEIGHT * 4);

    scaler = (struct scaler_ctx*)calloc(1, sizeof(*scaler));
}


void mister_set_mode(sr_mode *srm)
{
   // Check if mode is the same as previous
   if (memcmp(&mister_mode, srm, sizeof(sr_mode)) == 0)
      return;

   // otherwise store it
   memcpy(&mister_mode, srm, sizeof(sr_mode));

   // Signal mode switch pending
   mode_switch_pending = 1;
}

void mister_CmdSwitchres(sr_mode *srm)
{
/*
   if (!mister_video.isConnected)
      return;

   if (w < 200 || h < 160)
      return;

   if (w == mister_video.width_core && h == mister_video.height_core && vfreq == mister_video.vfreq_core)
      return;

   unsigned char retSR;
   sr_mode swres_result;
   int sr_mode_flags = 0;

   if (h > 288)
      sr_mode_flags = SR_MODE_INTERLACED;

   if (orientation)
      sr_mode_flags = sr_mode_flags | SR_MODE_ROTATED;

   RARCH_LOG("[MiSTer] Video_SetSwitchres - (in %dx%d@%f)\n", w, h, vfreq);

   if (h < 288 && !mister_is15()) h = h << 1; //fix width
   if (h > 288 && h <= 400 && mister_is15()) h = 480; //fix height dosbox-pure

   retSR = sr_add_mode(w, h, vfreq, sr_mode_flags, &swres_result);
*/
   if (srm == 0)
      return;

   RARCH_LOG("[MiSTer] Video_SetSwitchres - (result %dx%d@%f) - x=%.4f y=%.4f stretched(%d)\n", srm->width, srm->height,srm->vfreq, srm->x_scale, srm->y_scale, srm->is_stretched);

   char buffer[26];

   double px = (double) srm->pclock / 1000000.0;
   uint16_t udp_hactive = srm->width;
   uint16_t udp_hbegin = srm->hbegin;
   uint16_t udp_hend = srm->hend;
   uint16_t udp_htotal = srm->htotal;
   uint16_t udp_vactive = srm->height;
   uint16_t udp_vbegin = srm->vbegin;
   uint16_t udp_vend = srm->vend;
   uint16_t udp_vtotal = srm->vtotal;
   uint8_t  udp_interlace = srm->interlace;

   mister_video.width = udp_hactive;
   mister_video.height = udp_vactive;
   mister_video.lines_padding = 0; //(udp_vactive > h && udp_vactive != h * 2) ? udp_vactive - h : 0;
   mister_video.vfreq = srm->refresh;
   mister_video.lines = udp_vtotal;
   mister_video.interlaced = udp_interlace;
   mister_video.downscaled = 0;

   mister_video.widthTime = round((double) udp_htotal * (1 / px)); //in usec, time to raster 1 line
   mister_video.frameTime = mister_video.widthTime * udp_vtotal;

   if (mister_video.interlaced)
   {
      mister_video.frameField = 0;
      mister_video.frameTime = mister_video.frameTime >> 1;
   }
/*
   if (h > mister_video.height)
   {
      mister_video.downscaled = 1;
   }
*/
   RARCH_LOG("[MiSTer] Sending CMD_SWITCHRES...\n");

   buffer[0] = mister_CMD_SWITCHRES;
   memcpy(&buffer[1],&px,sizeof(px));
   memcpy(&buffer[9],&udp_hactive,sizeof(udp_hactive));
   memcpy(&buffer[11],&udp_hbegin,sizeof(udp_hbegin));
   memcpy(&buffer[13],&udp_hend,sizeof(udp_hend));
   memcpy(&buffer[15],&udp_htotal,sizeof(udp_htotal));
   memcpy(&buffer[17],&udp_vactive,sizeof(udp_vactive));
   memcpy(&buffer[19],&udp_vbegin,sizeof(udp_vbegin));
   memcpy(&buffer[21],&udp_vend,sizeof(udp_vend));
   memcpy(&buffer[23],&udp_vtotal,sizeof(udp_vtotal));
   memcpy(&buffer[25],&udp_interlace,sizeof(udp_interlace));
   mister_Send(&buffer[0], 26);

   modeline_active = 1;
   mode_switch_pending = 0;
}

void mister_CmdBlit(char *bufferFrame, uint16_t vsync)
{
   if (!mister_video.isConnected)
      return;

   char buffer[9];

   mister_video.frame++;

   // 16 or 32 lines blockSize
   uint8_t blockLinesFactor = (mister_video.width > 384) ? 5   : 4;

   // Compressed blocks are 16/32 lines long
   uint32_t blockSize = (mister_video.lz4_compress) ? (mister_video.width << blockLinesFactor) * 3 : 0;

   if (blockSize > mister_MAX_LZ4_BLOCK)
      blockSize = mister_MAX_LZ4_BLOCK;

   // Manual vsync
   if (vsync != 0)
   {
      mister_video.vsync_auto = vsync;
   }

   buffer[0] = mister_CMD_BLIT_VSYNC;
   memcpy(&buffer[1], &mister_video.frame, sizeof(mister_video.frame));
   memcpy(&buffer[5], &mister_video.vsync_auto, sizeof(mister_video.vsync_auto));
   buffer[7] = (uint16_t) blockSize & 0xff;
   buffer[8] = (uint16_t) blockSize >> 8;

   mister_Send(&buffer[0], 9);

   uint32_t bufferSize = (mister_video.interlaced == 0) ? mister_video.width * mister_video.height * 3 : mister_video.width * (mister_video.height >> 1) * 3;

   //bufferSize += (mister_video.width * mister_video.lines_padding * 3) >>  mister_video.interlaced; //hack when switchres and core resolution not match

   if (mister_video.lz4_compress == false)
      mister_SendMTU(&bufferFrame[0], bufferSize, 1470);
   else
      mister_SendLZ4(&bufferFrame[0], bufferSize, blockSize);
}

void mister_CmdAudio(const void *bufferFrame, uint32_t sizeSound, uint8_t soundchan)
{
   if (!mister_video.isConnected)
      return;

   if (!mister_video.fpga_audio)
      return;

   char buffer[3];
   buffer[0] = mister_CMD_AUDIO;

   uint16_t bytesSound = sizeSound * soundchan * 2;

   memcpy(&buffer[1], &bytesSound, sizeof(bytesSound));

   mister_Send(&buffer[0], 3);
   const uint8_t *data_in = (const uint8_t *)bufferFrame;

   mister_SendMTU((char *) &data_in[0], bytesSound, 1472);
}

void mister_setBlitTime(retro_time_t blitTime)
{
   if (mister_video.frame > 10) //first frames spends more time as usual
   {
      mister_video.avgBlitTime = (mister_video.avgBlitTime == 0) ? blitTime: (mister_video.avgBlitTime + blitTime) / 2;
   }
   else
   {
      mister_video.avgBlitTime = 0;
   }
}

int mister_Sync(retro_time_t emulationTime)
{
   if (!mister_video.isConnected)
      return 0;

   if (mister_video.frame > 10) //first frames spends more time as usual
   {
      mister_video.avgEmulationTime = (mister_video.avgEmulationTime == 0) ? emulationTime + mister_video.avgBlitTime: (mister_video.avgEmulationTime + emulationTime + mister_video.avgBlitTime) / 2;
      mister_video.vsync_auto = mister_video.height - round(mister_video.lines * mister_video.avgEmulationTime) / mister_video.frameTime; //vblank for desviation
      if (mister_video.vsync_auto > 480) mister_video.vsync_auto = 1;
   }
   else
   {
      mister_video.avgEmulationTime = 0;
      mister_video.vsync_auto = 120;
   }

   int diffTime = mister_GetVSyncDif();  //adjusting time with raster
   if (diffTime > 60000 || diffTime < -60000)
      return 0;
   else
      return diffTime;
}

int mister_GetVSyncDif(void)
{
   uint32_t prevFrameEcho = mister_video.frameEcho;
   int diffTime = 0;

   if (mister_video.frame != mister_video.frameEcho) //some ack is pending
   {
      mister_ReceiveBlitACK();
   }

   if (prevFrameEcho != mister_video.frameEcho) //if ack is updated, check raster difference
   {
      //horror patch if emulator freezes to align frame counter
      if ((mister_video.frameEcho + 1) < mister_video.frameGPU)
      {
         mister_video.frame = mister_video.frameGPU + 1;
      }

      uint32_t vcount1 = ((mister_video.frameEcho - 1) * mister_video.lines + mister_video.vcountEcho) >> mister_video.interlaced;
      uint32_t vcount2 = (mister_video.frameGPU * mister_video.lines + mister_video.vcountGPU) >> mister_video.interlaced;
      int dif = (int)(vcount1 - vcount2) / 2;   //dicotomic

      diffTime = (int)(mister_video.widthTime * dif);

      //printf("echo %d %d / %d %d (dif (vc1=%d,vc2=%d) %d, diffTime=%d)\n", mister_video.frameEcho, mister_video.vcountEcho, mister_video.frameGPU, mister_video.vcountGPU,vcount1,vcount2,dif,diffTime);
   }

   return diffTime;
}


bool mister_is15(void)
{
   sr_state swres_state;
   sr_get_state(&swres_state);

   return (!strcmp("arcade_15",swres_state.monitor) || !strcmp("generic_15",swres_state.monitor) || !strcmp("ntsc",swres_state.monitor) || !strcmp("pal",swres_state.monitor) || !strcmp("arcade_15_25",swres_state.monitor) || !strcmp("arcade_15_25_31",swres_state.monitor));
}

bool mister_isInterlaced(void)
{
   return mister_video.interlaced;
}

bool mister_is480p(void)
{
   return (!mister_video.interlaced && (mister_video.height > 288 || mister_video.height_core == mister_video.height >> 1));
}

bool mister_isDownscaled(void)
{
   return mister_video.downscaled;
}

int mister_GetField(void)
{
   int field = 0;
   if (mister_video.interlaced)
   {
      mister_video.frameField = !mister_video.frameField;
      field = mister_video.frameField;
   }

   return field;
}

uint16_t mister_GetWidth(void)
{
   return mister_video.width;
}

uint16_t mister_GetHeight(void)
{
   return mister_video.height;
}
//Private
void mister_Send(void *cmd, int cmdSize)
{
   sendto(mister_video.sockfd, (char *) cmd, cmdSize, 0, (struct sockaddr *)&mister_video.ServerAddr, sizeof(mister_video.ServerAddr));
}


void mister_SendMTU(char *buffer, int bytes_to_send, int chunk_max_size)
{
   int bytes_this_chunk = 0;
   int chunk_size = 0;
   uint32_t offset = 0;

   do
   {
      chunk_size = bytes_to_send > chunk_max_size? chunk_max_size : bytes_to_send;
      bytes_to_send -= chunk_size;
      bytes_this_chunk = chunk_size;

      mister_Send(buffer + offset, bytes_this_chunk);
      offset += chunk_size;

   } while (bytes_to_send > 0);
}

void mister_SendLZ4(char *buffer, int bytes_to_send, int block_size)
{
   LZ4_stream_t lz4_stream_body;
   LZ4_stream_t* lz4_stream = &lz4_stream_body;
   LZ4_initStream(lz4_stream, sizeof(*lz4_stream));
/*
   LZ4_streamHC_t lz4_streamHC_body;
   LZ4_streamHC_t* lz4_streamHC = &lz4_streamHC_body;
   LZ4_initStreamHC(lz4_streamHC, sizeof(*lz4_streamHC));
*/
   int inp_buf_index = 0;
   int bytes_this_chunk = 0;
   int chunk_size = 0;
   uint32_t offset = 0;

   do
   {
      chunk_size = bytes_to_send > block_size? block_size : bytes_to_send;
      bytes_to_send -= chunk_size;
      bytes_this_chunk = chunk_size;

      char* const inp_ptr = mister_video.inp_buf[inp_buf_index];
      memcpy((char *)&inp_ptr[0], buffer + offset, chunk_size);

      const uint16_t c_size = LZ4_compress_fast_continue(lz4_stream, inp_ptr, (char *)&mister_video.m_fb_compressed[2], bytes_this_chunk, sizeof(mister_video.m_fb_compressed), 1);
      //const uint16_t c_size = LZ4_compress_HC_continue(lz4_streamHC, inp_ptr, (char *)&m_fb_compressed[2], bytes_this_chunk, sizeof(m_fb_compressed));

      uint16_t *c_size_ptr = (uint16_t *)&mister_video.m_fb_compressed[0];
      *c_size_ptr = c_size;

      mister_SendMTU((char *) &mister_video.m_fb_compressed[0], c_size + 2, 1472);
      offset += chunk_size;
      inp_buf_index ^= 1;

   } while (bytes_to_send > 0);
}

void mister_ReceiveBlitACK(void)
{
   uint32_t frameUDP = mister_video.frameEcho;
   socklen_t sServerAddr = sizeof(struct sockaddr);
   int len = 0;
   do
   {
      len = recvfrom(mister_video.sockfd, mister_video.bufferRecv, sizeof(mister_video.bufferRecv), 0, (struct sockaddr *)&mister_video.ServerAddr, &sServerAddr);
      if (len > 0)
      {
         memcpy(&frameUDP, &mister_video.bufferRecv[0],4);
         if (frameUDP > mister_video.frameEcho)
         {
            mister_video.frameEcho = frameUDP;
            memcpy(&mister_video.vcountEcho, &mister_video.bufferRecv[4],2);
            memcpy(&mister_video.frameGPU, &mister_video.bufferRecv[6],4);
            memcpy(&mister_video.vcountGPU, &mister_video.bufferRecv[10],2);
            memcpy(&mister_video.fpga_debug_bits, &mister_video.bufferRecv[12],1);

            bitByte bits;
            bits.byte = mister_video.fpga_debug_bits;
            mister_video.fpga_vram_ready     = bits.u.bit0;
            mister_video.fpga_vram_end_frame = bits.u.bit1;
            mister_video.fpga_vram_synced    = bits.u.bit2;
            mister_video.fpga_vga_frameskip  = bits.u.bit3;
            mister_video.fpga_vga_vblank     = bits.u.bit4;
            mister_video.fpga_vga_f1         = bits.u.bit5;
            mister_video.fpga_audio          = bits.u.bit6;
            mister_video.fpga_vram_queue     = bits.u.bit7;
            //printf("ReceiveBlitACK %d %d / %d %d \n", frameEcho, vcountEcho, frameGPU, vcountGPU);
         }
      }
   } while (len > 0 && mister_video.frame != mister_video.frameEcho);
}
