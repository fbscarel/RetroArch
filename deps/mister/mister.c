#include "mister.h"

static mister_video_t mister_video;
static sr_mode mister_mode;
static char *texture_frame = 0;
static unsigned menu_width = 0;
static unsigned menu_height = 0;
static bool modeline_active = 0;
static bool mode_switch_pending = 0;

void mister_set_texture_frame(char *frame, unsigned width, unsigned height)
{
   texture_frame = frame;
   menu_width = width;
   menu_height = height;
}

void mister_draw(video_driver_state_t *video_st, const void *data, unsigned width, unsigned height, size_t pitch)
{
   settings_t *settings  = config_get_ptr();
   struct retro_system_av_info *av_info   = &video_st->av_info;
   const struct retro_system_timing *info = (const struct retro_system_timing*)&av_info->timing;
   audio_driver_state_t *audio_st  = audio_state_get_ptr();
   bool blitMister = false;
   bool menu_on = false;

   if (retroarch_get_rotation() & 1)
   {
      unsigned tmp = width;
      width = height;
      height = tmp;
   }

#ifdef HAVE_MENU
   struct menu_state *menu_st              = menu_state_get_ptr();
   if (menu_st->flags & MENU_ST_FLAG_ALIVE && texture_frame != 0)
   {
      menu_on = true;
      width = menu_width;
      height = menu_height * (mister_isInterlaced() ? 2 : 1);
      pitch = width * sizeof(uint16_t);
   }
#endif

   if (settings->bools.video_mister_enable && audio_st->output_mister && audio_st->output_mister_samples)
   {
      mister_CmdAudio(audio_st->output_mister_samples_conv_buf, audio_st->output_mister_samples >> 1, 2);
      audio_st->output_mister_samples = 0;
   }

   if (settings->bools.video_mister_enable && video_st->frame_count > 0 && !blitMister)
   {
      if (!mister_video.isConnected)
         mister_CmdInit(settings->arrays.mister_ip, 32100, settings->bools.mister_lz4, settings->uints.audio_output_sample_rate, 2);

      if (mode_switch_pending)
         mister_CmdSwitchres(&mister_mode);

      if (!modeline_active)
         return;

      retro_time_t mister_bt1  = cpu_features_get_time_usec();

      uint32_t totalPixels = height * width;
      uint32_t numPix = 0;

      if (mister_is480p() && height < 480)
      {
         totalPixels = totalPixels << 1;
      }

      if ((mister_isInterlaced() && height > 288) || (mister_isDownscaled()))
      {
         totalPixels = totalPixels >> 1;
      }

      uint8_t *mister_buffer = (uint8_t*)malloc(1024 * 768 * 3);
      unsigned c = 0;

      uint8_t field = 0;

      if (!blitMister && mister_buffer && (video_st->frame_cache_data != RETRO_HW_FRAME_BUFFER_VALID || menu_on) && pitch > 0) //software rendered
      {
         field = mister_GetField();
         blitMister = true;
         union
         {
            const uint8_t *u8;
            const uint16_t *u16;
            const uint32_t *u32;
         } u;

         u.u8 = menu_on? (const uint8_t*)texture_frame : (const uint8_t*)data;

         int x_start = mister_video.width > width ? (mister_video.width - width) / 2 : 0;
         int x_crop = mister_video.width < width ? width - mister_video.width : 0;
         int y_start = mister_video.height > height ? (mister_video.height - height) / 2 : 0;
         int y_crop = mister_video.height < height ? (height - mister_video.height) / (mister_isInterlaced() ? 2 : 1) : 0;

         u.u8 += (y_crop / 2) * pitch + (x_crop / 2);

         for (u_int j = field; j < height - y_crop; j++, u.u8 += pitch)
         {
            c = ((j + y_start) * mister_video.width + x_start) * 3;

            for (u_int i = 0; i < width - x_crop; i++)
            {
               if (menu_on)
               {
                  uint16_t pixel = u.u16[i];
                  mister_buffer[c + 0] = (pixel >>  8); //b
                  mister_buffer[c + 1] = (pixel >>  4); //g
                  mister_buffer[c + 2] = (pixel >>  0); //r
               }

               else if (video_st->pix_fmt == RETRO_PIXEL_FORMAT_RGB565)
               {
                  uint16_t pixel = u.u16[i];
                  uint8_t r  = (pixel >> 11) & 0x1f;
                  uint8_t g  = (pixel >>  5) & 0x3f;
                  uint8_t b  = (pixel >>  0) & 0x1f;
                  mister_buffer[c + 0] = (b << 3) | (b >> 2);
                  mister_buffer[c + 1] = (g << 2) | (g >> 4);
                  mister_buffer[c + 2] = (r << 3) | (r >> 2);
               }
               else
               {
                  uint32_t pixel = u.u32[i];
                  mister_buffer[c + 0] = (pixel >>  0) & 0xff; //b
                  mister_buffer[c + 1] = (pixel >>  8) & 0xff; //g
                  mister_buffer[c + 2] = (pixel >> 16) & 0xff; //r
               }
               c += 3;
               numPix++;
               if (numPix >= totalPixels)
                  break;
            }
            if (numPix >= totalPixels)
               break;

            if (((mister_isInterlaced() && height > 288) || mister_isDownscaled()) && !menu_on)
               u.u8 += pitch;

            if (mister_is480p() && height < 480) //do scanlines for 31khz
            {
               for(uint32_t x = 0; x < width; x++)
               {
                  mister_buffer[c + 0] = 0x00;
                  mister_buffer[c + 1] = 0x00;
                  mister_buffer[c + 2] = 0x00;
                  c += 3;
                  numPix++;

                  if (numPix >= totalPixels)
                     break;
               }
               if (numPix >= totalPixels)
                  break;
            }
         }
      }

      if (!blitMister && mister_buffer && data == RETRO_HW_FRAME_BUFFER_VALID && video_st->frame_cache_data) //hardware rendered
      {
         struct video_viewport vp = { 0 };
         video_driver_get_viewport_info(&vp);
         uint16_t mister_width = mister_GetWidth();
         uint16_t mister_height = mister_GetHeight();

         if (vp.width != mister_width || vp.height != mister_height || video_st->frame_count == 1)
         {
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
         }

         video_driver_get_viewport_info(&vp);
         //printf("%d %d %d %d\n",mister_width,mister_height,vp.width,vp.height);
         uint8_t *bit24_image = (uint8_t*)malloc(vp.width*vp.height*3);

         if (bit24_image && vp.width == mister_width && vp.height == mister_height && video_st->current_video->read_viewport && video_st->current_video->read_viewport(video_st->data, bit24_image, false))
         {
            //printf("frame %d size %d width %d %d height %d %d pitch %d\n",video_st->frame_count , vp.width*vp.height*3, width, vp.width, height,vp.height, pitch);
            field = mister_GetField();
            blitMister = true;
            const uint8_t *u8 = (const uint8_t*)bit24_image;
            u8 += (vp.height * vp.width * 3) - 1;

            for(uint32_t i=field; i<vp.height; i++, u8 -= vp.width*3)
            {
               for (uint32_t j=0; j<vp.width;j++)
               {
                  uint32_t pix = j * 3;
                  mister_buffer[c + 0] = u8[pix+1];
                  mister_buffer[c + 1] = u8[pix+2];
                  mister_buffer[c + 2] = u8[pix];

                  c+=3;
                  numPix++;
                  if (numPix >= totalPixels)
                     break;
               }
               if (numPix >= totalPixels)
                  break;

               if ((mister_isInterlaced() && vp.height > 288) || mister_isDownscaled())
                 u8 -= vp.width*3;

               if (mister_is480p() && vp.height < 480) //do scanlines for 31khz
               {
                  for(uint32_t x = 0; x < vp.width; x++)
                  {
                     mister_buffer[c + 0] = 0x00;
                     mister_buffer[c + 1] = 0x00;
                     mister_buffer[c + 2] = 0x00;
                     c += 3;
                     numPix++;

                     if (numPix >= totalPixels)
                         break;
                  }
                  if (numPix >= totalPixels)
                    break;
               }
            }
         }
         free(bit24_image);
      }

      if (blitMister)
      {
         int mister_vsync = 1;
         if (settings->bools.video_frame_delay_auto && video_st->frame_delay_effective > 0)
         {
            mister_vsync = height / (16 / video_st->frame_delay_effective);
         }
         else if (settings->uints.video_frame_delay > 0)
         {
            mister_vsync = height / (16 / settings->uints.video_frame_delay);
         }
         mister_CmdBlit((char *)&mister_buffer[0], mister_vsync);
         retro_time_t mister_bt2  = cpu_features_get_time_usec();
         mister_setBlitTime(mister_bt2 - mister_bt1);
      }

      free(mister_buffer);
   }
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
      //mister_video.frameField = !mister_video.frameField;
      //field = mister_video.frameField;
      field = mister_video.fpga_vga_f1;
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
