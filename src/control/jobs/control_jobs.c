/*
    This file is part of darktable,
    copyright (c) 2010-2011 henrik andersson, johannes hanika

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <glib.h>
#include <glib/gstdio.h>

#include "common/collection.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_dng.h"
#include "common/similarity.h"
#include "common/exif.h"
#include "common/film.h"
#include "common/imageio_module.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/tags.h"
#include "control/conf.h"
#include "control/jobs/control_jobs.h"
#include "develop/develop.h"

#include "gui/gtk.h"

void dt_control_write_sidecar_files()
{
  dt_job_t j;
  dt_control_write_sidecar_files_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

void dt_control_write_sidecar_files_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "write sidecar files");
  job->execute = &dt_control_write_sidecar_files_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_init(t);
}

int32_t dt_control_write_sidecar_files_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  while(t)
  {
    imgid = (long int)t->data;
    dt_image_t *img = dt_image_cache_get(imgid, 'r');
    char dtfilename[520];
    dt_image_full_path(img->id, dtfilename, 512);
    char *c = dtfilename + strlen(dtfilename);
    sprintf(c, ".xmp");
    dt_exif_xmp_write(imgid, dtfilename);
    dt_image_cache_release(img, 'r');
    t = g_list_delete_link(t, t);
  }
  return 0;
}


#define _INDEXER_UPDATE_HISTOGRAM		1
#define _INDEXER_UPDATE_LIGHTMAP		2
#define _INDEXER_IMAGE_FILE_REMOVED		4

typedef struct _control_indexer_img_t
{
  uint32_t id;
  uint32_t flags;
} _control_indexer_img_t;

int32_t dt_control_indexer_job_run(dt_job_t *job)
{
  
  /* 
   * First pass run thru ALL images and collect the ones who needs to update
   *  \TODO in the future lets have a indexer table with ids filed with images
   *  thats need some kind of reindexing.. all mark dirty functions adds image
   *  to this table--
   */
  GList *images=NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select images.id,film_rolls.folder||'/'||images.filename,images.histogram,images.lightmap from images,film_rolls where film_rolls.id = images.film_id", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    _control_indexer_img_t *idximg=g_malloc(sizeof( _control_indexer_img_t));
    memset(idximg,0,sizeof(_control_indexer_img_t));
    idximg->id = sqlite3_column_int(stmt,0);

    /* first check if image file exists on disk */
    const char *filename = (const char *)sqlite3_column_text(stmt, 1);
    if (filename && !g_file_test(filename, G_FILE_TEST_IS_REGULAR))
      idximg->flags |= _INDEXER_IMAGE_FILE_REMOVED;
   
    
    /* check if histogram should be updated */
    if (sqlite3_column_bytes(stmt, 2) != sizeof(dt_similarity_histogram_t))
      idximg->flags |= _INDEXER_UPDATE_HISTOGRAM;

    /* check if lightmap should be updated */
    if (sqlite3_column_bytes(stmt, 3) != sizeof(dt_similarity_lightmap_t))
      idximg->flags |= _INDEXER_UPDATE_LIGHTMAP;
    

    /* if image is flagged add to collection */
    if (idximg->flags != 0)
      images = g_list_append(images, idximg);
    else
      g_free(idximg);
  }
  sqlite3_finalize(stmt);
  
  
  /*
   * Second pass, run thru collected images thats
   *  need reindexing...
   */
  GList *imgitem = g_list_first(images);
  if(imgitem)
  {
    char message[512]= {0};
    double fraction=0;
    int total = g_list_length(images);

    /* background job plate only if more then one image is reindexed */
    snprintf(message, 512, ngettext ("re-indexing %d image", "re-indexing %d images", total), total );
    const guint jid = 0;
    if(total>1)
      jid = dt_control_backgroundjobs_create(darktable.control, 0, message);
    
    do {
      /* get the _control_indexer_img_t pointer */
      _control_indexer_img_t *idximg = imgitem->data;
      
      /*
       * Check if image has been delete from disk
       */
      if ((idximg->flags&_INDEXER_IMAGE_FILE_REMOVED))
      {
        /* file does not exist on disk lets delete image reference from database */
        //char query[512]={0};
        
        // \TODO dont delete move to an temp table and let user to revalidate
        
        /*sprintf(query,"delete from history where imgid=%d",idximg->id); 
        DT_DEBUG_SQLITE3_EXEC(darktable.db, query, NULL, NULL, NULL);
        sprintf(query,"delete from tagged_images where imgid=%d",idximg->id); 
        DT_DEBUG_SQLITE3_EXEC(darktable.db, query, NULL, NULL, NULL);
        sprintf(query,"delete from images where id=%d",idximg->id); 
        DT_DEBUG_SQLITE3_EXEC(darktable.db, query, NULL, NULL, NULL);*/
      
        /* no need to additional work */
        continue;
      }
      
      /*
       *  Check if image histogram or lightmap should be updated.
       *   those indexing that involves a image pipe should fall into this
       */
      if ( (idximg->flags&_INDEXER_UPDATE_HISTOGRAM) ||  (idximg->flags&_INDEXER_UPDATE_LIGHTMAP) )
      {
        /* get image from imgid */
        dt_image_t *img = dt_image_cache_get(idximg->id, 'r');
        
        /* setup a pipe Max x Height 320x320 */
        dt_develop_t dev;
        dt_dev_init(&dev, 0);
        dt_dev_load_image(&dev, img);
        
        /* check that we actually got image pixels... 
            if not this will SIGKILL down the pipe...
        */
        if(dev.image->pixels)
        {
          const int wd = dev.image->width;
          const int ht = dev.image->height;
          
          dt_dev_pixelpipe_t pipe;
          dt_dev_pixelpipe_init_export(&pipe, wd, ht);
          dt_dev_pixelpipe_set_input(&pipe, &dev, dev.image->pixels, dev.image->width, dev.image->height, 1.0);
          dt_dev_pixelpipe_create_nodes(&pipe, &dev);
          dt_dev_pixelpipe_synch_all(&pipe, &dev);
          dt_dev_pixelpipe_get_dimensions(&pipe, &dev, pipe.iwidth, pipe.iheight, &pipe.processed_width, &pipe.processed_height);
              
          /* set scaling */
          const float scalex = fminf(320/(float)pipe.processed_width,  1.0);
          const float scaley = fminf(320/(float)pipe.processed_height, 1.0);
          const float scale = fminf(scalex, scaley);
          const int dest_width=pipe.processed_width*scale, dest_height=pipe.processed_height*scale;
          
          /* process the pipe */
          dt_dev_pixelpipe_process(&pipe, &dev, 0, 0, dest_width, dest_height, scale);
          
          /* lets generate a histogram */
          uint8_t *pixel = pipe.backbuf;
        
          /*
           * Generate similarity histogram data if requested
           */
          if (pipe.backbuf && (idximg->flags&_INDEXER_UPDATE_HISTOGRAM))
          {
            dt_similarity_histogram_t histogram;
            float bucketscale = (float)DT_SIMILARITY_HISTOGRAM_BUCKETS/(float)0xff;
            for(int j=0;j<(4*dest_width*dest_height);j+=4)
            {
              /* swap rgb and scale to bucket index*/
              uint8_t rgb[3];
              
              for(int k=0; k<3; k++)
                rgb[k] = (int)((float)pixel[j+2-k] * bucketscale);

              /* distribute rgb into buckets */
              for(int k=0; k<3; k++)
                histogram.rgbl[rgb[k]][k]++;
              
              /* distribute lum into buckets */
              uint8_t lum = MAX(MAX(rgb[0], rgb[1]), rgb[2]);
              histogram.rgbl[lum][3]++;
            }
             
            for(int k=0; k<DT_SIMILARITY_HISTOGRAM_BUCKETS; k++) 
              for (int j=0;j<4;j++) 
                histogram.rgbl[k][j] /= (dest_width*dest_height);
              
            /* store the histogram data */
            dt_similarity_histogram_store(idximg->id, &histogram);
            
          }                  
          
          /*
           * Generate scaledowned similarity lightness map if requested
           */
          if (pipe.backbuf && (idximg->flags&_INDEXER_UPDATE_LIGHTMAP))
          {
            dt_similarity_lightmap_t lightmap;
            memset(&lightmap,0,sizeof(dt_similarity_lightmap_t));
            
            /* 
             * create a pixbuf out of the image for downscaling 
             */
            
            /* first of setup a standard rgb buffer, swap bgr in same routine */
            uint8_t *rgbbuf = g_malloc(dest_width*dest_height*3);
            for(int j=0;j<(dest_width*dest_height);j++)
              for(int k=0;k<3;k++)
                rgbbuf[3*j+k] = pixel[4*j+2-k];
           
            /* then create pixbuf and scale down to lightmap size */
            GdkPixbuf *source = gdk_pixbuf_new_from_data(rgbbuf,GDK_COLORSPACE_RGB,FALSE,8,dest_width,dest_height,(dest_width*3),NULL,NULL);
            GdkPixbuf *scaled = gdk_pixbuf_scale_simple(source,DT_SIMILARITY_LIGHTMAP_SIZE,DT_SIMILARITY_LIGHTMAP_SIZE,GDK_INTERP_HYPER);
            
            /* copy scaled data into lightmap */
            uint8_t min=0xff,max=0;
            uint8_t *spixels = gdk_pixbuf_get_pixels(scaled);
          
            for(int j=0;j<(DT_SIMILARITY_LIGHTMAP_SIZE*DT_SIMILARITY_LIGHTMAP_SIZE);j++)
            {
              /* copy rgb */
              for(int k=0;k<3;k++)
                lightmap.pixels[4*j+k] = spixels[3*j+k];
              
              /* average intensity into 4th channel */
              lightmap.pixels[4*j+3] =  (lightmap.pixels[4*j+0]+ lightmap.pixels[4*j+1]+ lightmap.pixels[4*j+2])/3.0;
              min = MAX(0,MIN(min, lightmap.pixels[4*j+3]));
              max = MIN(0xff,MAX(max, lightmap.pixels[4*j+3]));
            }
      
            /* contrast stretch each channel in lightmap */
            float scale=0;
            int range = max-min;
            if(range==0) 
              scale = 1.0;
            else 
              scale = 0xff/range;
            for(int j=0;j<(DT_SIMILARITY_LIGHTMAP_SIZE*DT_SIMILARITY_LIGHTMAP_SIZE);j++)
            {
              for(int k=0;k<4;k++)
                lightmap.pixels[4*j+k] = (lightmap.pixels[4*j+k]-min)*scale;
            }
              
            /* free some resources */
            gdk_pixbuf_unref(scaled);
            gdk_pixbuf_unref(source);
            
            g_free(rgbbuf);
            
            /* store the lightmap */
            dt_similarity_lightmap_store(idximg->id, &lightmap);
          }
    
    
          dt_dev_pixelpipe_cleanup(&pipe);
        }
        
        dt_dev_cleanup(&dev);
        dt_image_cache_release(img, 'r');
      }
      
      /* update background progress */
      if (total>1)
      {
	fraction+=1.0/total;
	dt_control_backgroundjobs_progress(darktable.control, jid, fraction);
      }
      
    } while ((imgitem=g_list_next(imgitem)) && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED);
    
    
    /* cleanup */
    if (total>1)
      dt_control_backgroundjobs_destroy(darktable.control, jid);
  }
  
  
  /* 
   * Indexing opertions finished, lets reschedule the indexer 
   * unless control is shutting down...
   */
  if(dt_control_running())
    dt_control_start_indexer();
  
  return 0;
}


typedef struct _control_match_similar_job_param_t {
  uint32_t imgid;
  dt_similarity_t data;
} _control_match_similar_job_param_t;

int32_t dt_control_match_similar_job_run(dt_job_t *job)
{
  _control_match_similar_job_param_t *t = (_control_match_similar_job_param_t *)job->param;
  dt_similarity_match_image(t->imgid, &t->data);
  return 0;
}
  
int32_t dt_control_merge_hdr_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  int total = g_list_length(t);
  char message[512]= {0};
  double fraction=0;
  snprintf(message, 512, ngettext ("merging %d image", "merging %d images", total), total );

  const guint jid = dt_control_backgroundjobs_create(darktable.control, 1, message); 
 
  float *pixels = NULL;
  float *weight = NULL;
  int wd = 0, ht = 0, first_imgid = -1;
  uint32_t filter = 0;
  float whitelevel = 0.0f;
  total ++;
  while(t)
  {
    imgid = (long int)t->data;
    dt_image_t *img = dt_image_cache_get(imgid, 'r');
    dt_image_buffer_t mip = dt_image_get_blocking(img, DT_IMAGE_FULL, 'r');
    if(img->filters == 0 || img->bpp != sizeof(uint16_t))
    {
      dt_control_log(_("exposure bracketing only works on raw images"));
      dt_image_release(img, DT_IMAGE_FULL, 'r');
      dt_image_cache_release(img, 'r');
      free(pixels);
      free(weight);
      goto error;
    }
    filter = dt_image_flipped_filter(img);
    if(mip != DT_IMAGE_FULL)
    {
      dt_control_log(_("failed to get raw buffer from image `%s'"), img->filename);
      dt_image_cache_release(img, 'r');
      free(pixels);
      free(weight);
      goto error;
    }

    if(!pixels)
    {
      first_imgid = imgid;
      pixels = (float *)malloc(sizeof(float)*img->width*img->height);
      weight = (float *)malloc(sizeof(float)*img->width*img->height);
      memset(pixels, 0x0, sizeof(float)*img->width*img->height);
      memset(weight, 0x0, sizeof(float)*img->width*img->height);
      wd = img->width;
      ht = img->height;
    }
    else if(img->width != wd || img->height != ht)
    {
      dt_control_log(_("images have to be of same size!"));
      free(pixels);
      free(weight);
      dt_image_release(img, DT_IMAGE_FULL, 'r');
      dt_image_cache_release(img, 'r');
      goto error;
    }
    // if no valid exif data can be found, assume peleng fisheye at f/16, 8mm, with half of the light lost in the system => f/22
    const float eap = img->exif_aperture > 0.0f ? img->exif_aperture : 22.0f;
    const float efl = img->exif_focal_length > 0.0f ? img->exif_focal_length : 8.0f;
    const float rad = .5f * efl/eap;
    const float aperture = M_PI * rad * rad;
    const float iso = img->exif_iso > 0.0f ? img->exif_iso : 100.0f;
    const float exp = img->exif_exposure > 0.0f ? img->exif_exposure : 1.0f;
    const float cal = 100.0f/(aperture*exp*iso);
    whitelevel = fmaxf(whitelevel, cal);
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) default(none) shared(img, pixels, weight, wd, ht)
#endif
    for(int k=0; k<wd*ht; k++)
    {
      const uint16_t in = ((uint16_t *)img->pixels)[k];
      const float w = .001f + (in >= 1000 ? (in < 65000 ? in/65000.0f : 0.0f) : exp * 0.01f);
      pixels[k] += w * in * cal;
      weight[k] += w;
    }

    t = g_list_delete_link(t, t);
    
    /* update backgroundjob ui plate */
    fraction+=1.0/total;
    dt_control_backgroundjobs_progress(darktable.control, jid, fraction);

    dt_image_release(img, DT_IMAGE_FULL, 'r');
    dt_image_cache_release(img, 'r');
  }
  // normalize by white level to make clipping at 1.0 work as expected (to be sure, scale down one more stop, thus the 0.5):
#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(pixels, wd, ht, weight, whitelevel)
#endif
  for(int k=0; k<wd*ht; k++) pixels[k] = fmaxf(0.0f, fminf(2.0f, pixels[k]/((.5f*whitelevel*65535.0f)*weight[k])));

  // output hdr as digital negative with exif data.
  uint8_t exif[65535];
  char pathname[1024];
  dt_image_full_path(first_imgid, pathname, 1024);
  const int exif_len = dt_exif_read_blob(exif, pathname, 0, first_imgid);
  char *c = pathname + strlen(pathname);
  while(*c != '.' && c > pathname) c--;
  g_strlcpy(c, "-hdr.dng", sizeof(pathname)-(c-pathname));
  dt_imageio_write_dng(pathname, pixels, wd, ht, exif, exif_len, filter, whitelevel);
  
  dt_control_backgroundjobs_progress(darktable.control, jid, 1.0f);

  while(*c != '/' && c > pathname) c--;
  dt_control_log(_("wrote merged hdr `%s'"), c+1);

  // import new image
  gchar *directory = g_path_get_dirname((const gchar *)pathname);
  dt_film_t film;
  const int filmid = dt_film_new(&film, directory);
  dt_image_import(filmid, pathname, TRUE);
  g_free (directory);

  free(pixels);
  free(weight);
error:
  dt_control_backgroundjobs_destroy(darktable.control, jid);
  return 0;
}

int32_t dt_control_duplicate_images_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  int total = g_list_length(t);
  char message[512]= {0};
  double fraction=0;
  snprintf(message, 512, ngettext ("duplicating %d image", "duplicating %d images", total), total );
  const guint jid = dt_control_backgroundjobs_create(darktable.control, 0, message);
  while(t)
  {
    imgid = (long int)t->data;
    dt_image_duplicate(imgid);
    t = g_list_delete_link(t, t);
    fraction=1.0/total;
    dt_control_backgroundjobs_progress(darktable.control, jid, fraction);
  }
  dt_control_backgroundjobs_destroy(darktable.control, jid);
  return 0;
}

int32_t dt_control_flip_images_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  const int cw = t1->flag;
  GList *t = t1->index;
  int total = g_list_length(t);
  char message[512]= {0};
  double fraction=0;
  snprintf(message, 512, ngettext ("flipping %d image", "flipping %d images", total), total );
  const guint jid = dt_control_backgroundjobs_create(darktable.control, 0, message);
  while(t)
  {
    imgid = (long int)t->data;
    dt_image_flip(imgid, cw);
    t = g_list_delete_link(t, t);
    fraction=1.0/total;
    dt_control_backgroundjobs_progress(darktable.control, jid, fraction);
  }
  dt_control_backgroundjobs_destroy(darktable.control, jid);
  return 0;
}

int32_t dt_control_remove_images_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  int total = g_list_length(t);
  char message[512]= {0};
  double fraction=0;
  snprintf(message, 512, ngettext ("removing %d image", "removing %d images", total), total );
  const guint jid = dt_control_backgroundjobs_create(darktable.control, 0, message);

  char query[1024];
  sprintf(query, "update images set flags = (flags | %d) where id in (select imgid from selected_images)",DT_IMAGE_REMOVE);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);

  dt_collection_update(darktable.collection);
  dt_control_queue_redraw();

  // We need a list of files to regenerate .xmp files if there are duplicates
  GList *list = NULL;
  sqlite3_stmt *stmt = NULL;
  
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select distinct folder || '/' || filename from images, film_rolls where images.film_id = film_rolls.id and images.id in (select imgid from selected_images)", -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    list = g_list_append(list, g_strdup((const gchar *)sqlite3_column_text(stmt, 0)));
  }
  sqlite3_finalize(stmt);

  while(t)
  {
    imgid = (long int)t->data;
    dt_image_remove(imgid);
    t = g_list_delete_link(t, t);
    fraction=1.0/total;
    dt_control_backgroundjobs_progress(darktable.control, jid, fraction);
  }

  char *imgname;
  while(list)
  {
    imgname = (char *)list->data;
    dt_image_synch_all_xmp(imgname);
    list = g_list_delete_link(list, list);
  } 
  g_list_free(list);  
  dt_control_backgroundjobs_destroy(darktable.control, jid);
  dt_film_remove_empty();
  return 0;
}


int32_t dt_control_delete_images_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  int total = g_list_length(t);
  char message[512]= {0};
  double fraction=0;
  snprintf(message, 512, ngettext ("deleting %d image", "deleting %d images", total), total );
  const guint jid = dt_control_backgroundjobs_create(darktable.control, 0, message);

  sqlite3_stmt *stmt;

  char query[1024];
  sprintf(query, "update images set flags = (flags | %d) where id in (select imgid from selected_images)",DT_IMAGE_REMOVE);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);

  dt_collection_update(darktable.collection);
  dt_control_queue_redraw();

  // We need a list of files to regenerate .xmp files if there are duplicates
  GList *list = NULL;
  
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select distinct folder || '/' || filename from images, film_rolls where images.film_id = film_rolls.id and images.id in (select imgid from selected_images)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    list = g_list_append(list, g_strdup((const gchar *)sqlite3_column_text(stmt, 0)));
  }
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select count(id) from images where filename in (select filename from images where id = ?1) and film_id in (select film_id from images where id = ?1)", -1, &stmt, NULL);
  while(t)
  {
    imgid = (long int)t->data;
    char filename[512];
    dt_image_full_path(imgid, filename, 512);

    int duplicates = 0;
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
      duplicates = sqlite3_column_int(stmt, 0);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    // remove from disk:
    if(duplicates == 1) // don't remove the actual data if there are (other) duplicates using it
      (void)g_unlink(filename);
    dt_image_path_append_version(imgid, filename, 512);
    char *c = filename + strlen(filename);
    sprintf(c, ".xmp");
    (void)g_unlink(filename);
    sprintf(c, ".dt");
    (void)g_unlink(filename);
    sprintf(c, ".dttags");
    (void)g_unlink(filename);

    dt_image_remove(imgid);

    t = g_list_delete_link(t, t);
    fraction=1.0/total;
    dt_control_backgroundjobs_progress(darktable.control, jid, fraction);
  }
  sqlite3_finalize(stmt);
  
  char *imgname;
  while(list)
  {
    imgname = (char *)list->data;
    dt_image_synch_all_xmp(imgname);
    list = g_list_delete_link(list, list);
  } 
  g_list_free(list);
  dt_control_backgroundjobs_destroy(darktable.control, jid);
  dt_film_remove_empty();
  return 0;
}

void dt_control_image_enumerator_job_init(dt_control_image_enumerator_t *t)
{
  /* get sorted list of selected images */
  t->index = dt_collection_get_selected(darktable.collection);
}


void dt_control_merge_hdr_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "merge hdr image");
  job->execute = &dt_control_merge_hdr_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_init(t);
}

void dt_control_indexer_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "image indexer");
  job->execute = &dt_control_indexer_job_run;
}

void dt_control_match_similar_job_init(dt_job_t *job, uint32_t imgid,dt_similarity_t *data)
{
  dt_control_job_init(job, "match similar images");  
  job->execute = &dt_control_match_similar_job_run;
  _control_match_similar_job_param_t *t = (_control_match_similar_job_param_t *)job->param;
  t->imgid = imgid;
  t->data = *data;
}

void dt_control_duplicate_images_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "duplicate images");
  job->execute = &dt_control_duplicate_images_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_init(t);
}

void dt_control_flip_images_job_init(dt_job_t *job, const int32_t cw)
{
  dt_control_job_init(job, "flip images");
  job->execute = &dt_control_flip_images_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_init(t);
  t->flag = cw;
}

void dt_control_remove_images_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "remove images");
  job->execute = &dt_control_remove_images_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_init(t);
}

void dt_control_delete_images_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "delete images");
  job->execute = &dt_control_delete_images_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_init(t);
}

void dt_control_merge_hdr()
{
  dt_job_t j;
  dt_control_merge_hdr_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

void dt_control_match_similar(dt_similarity_t *data)
{
  dt_job_t j;
  GList *selected = dt_collection_get_selected(darktable.collection);
  if(selected)
  {
    dt_control_match_similar_job_init(&j, (long int)selected->data, data);
    dt_control_add_job(darktable.control, &j);
  } else
    dt_control_log(_("select an image as target for search of similar images"));
}

void dt_control_duplicate_images()
{
  dt_job_t j;
  dt_control_duplicate_images_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

void dt_control_flip_images(const int32_t cw)
{
  dt_job_t j;
  dt_control_flip_images_job_init(&j, cw);
  dt_control_add_job(darktable.control, &j);
}

void dt_control_remove_images()
{
  if(dt_conf_get_bool("ask_before_remove"))
  {
    GtkWidget *dialog;
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
    
    sqlite3_stmt *stmt = NULL;
    int number = 0;
    
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select count(imgid) from selected_images", -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      number = sqlite3_column_int(stmt, 0);
    }

    dialog = gtk_message_dialog_new(GTK_WINDOW(win),
                                    GTK_DIALOG_DESTROY_WITH_PARENT,
                                    GTK_MESSAGE_QUESTION,
                                    GTK_BUTTONS_YES_NO,
                                    ngettext("do you really want to remove %d selected image from the collection?", 
                                             "do you really want to remove %d selected images from the collection?", number), number);

    gtk_window_set_title(GTK_WINDOW(dialog), _("remove images?"));
    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if(res != GTK_RESPONSE_YES) return;
  }
  dt_job_t j;
  dt_control_remove_images_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

void dt_control_delete_images()
{
  if(dt_conf_get_bool("ask_before_delete"))
  {
    GtkWidget *dialog;
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

    sqlite3_stmt *stmt = NULL;
    int number = 0;
    
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select count(imgid) from selected_images", -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      number = sqlite3_column_int(stmt, 0);
    }

    dialog = gtk_message_dialog_new(GTK_WINDOW(win),
                                    GTK_DIALOG_DESTROY_WITH_PARENT,
                                    GTK_MESSAGE_QUESTION,
                                    GTK_BUTTONS_YES_NO,
                                    ngettext("do you really want to PHYSICALLY delete %d selected image from disk?",
                                             "do you really want to PHYSICALLY delete %d selected images from disk?", number), number);

    gtk_window_set_title(GTK_WINDOW(dialog), _("delete images?"));
    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if(res != GTK_RESPONSE_YES) return;
  }
  dt_job_t j;
  dt_control_delete_images_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

int32_t dt_control_export_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  const int total = g_list_length(t);
  int size = 0;
  dt_imageio_module_format_t  *mformat  = dt_imageio_get_format();
  g_assert(mformat);
  dt_imageio_module_storage_t *mstorage = dt_imageio_get_storage();
  g_assert(mstorage);

  // Get max dimensions...
  uint32_t w,h,fw,fh,sw,sh;
  fw=fh=sw=sh=0;
  mstorage->dimension(mstorage, &sw,&sh);
  mformat->dimension(mformat, &fw,&fh);

  if( sw==0 || fw==0) w=sw>fw?sw:fw;
  else w=sw<fw?sw:fw;

  if( sh==0 || fh==0) h=sh>fh?sh:fh;
  else h=sh<fh?sh:fh;

  // get shared storage param struct (global sequence counter, one picasa connection etc)
  dt_imageio_module_data_t *sdata = mstorage->get_params(mstorage, &size);
  if(sdata == NULL)
  {
    dt_control_log(_("failed to get parameters from storage module, aborting export.."));
    return 1;
  }
  dt_control_log(ngettext ("exporting %d image..", "exporting %d images..", total), total);
  char message[512]= {0};
  snprintf(message, 512, ngettext ("exporting %d image to %s", "exporting %d images to %s", total), total, mstorage->name() );
  
  /* create a cancellable bgjob ui template */
  const guint jid = dt_control_backgroundjobs_create(darktable.control, 0, message );
  dt_control_backgroundjobs_set_cancellable(darktable.control, jid, job);
  const dt_control_t *control = darktable.control;

  double fraction=0;
#ifdef _OPENMP
  // limit this to num threads = num full buffers - 1 (keep one for darkroom mode)
  // use min of user request and mipmap cache entries
  const int full_entries = dt_conf_get_int ("parallel_export");
  // GCC won't accept that this variable is used in a macro, considers
  // it set but not used, which makes for instance Fedora break.
  const __attribute__((__unused__)) int num_threads = MAX(1, MIN(full_entries, darktable.mipmap_cache->num_entries[DT_IMAGE_FULL]) - 1);
#pragma omp parallel default(none) private(imgid, size) shared(control,fraction, stderr, w, h, mformat, mstorage, t, sdata, job) num_threads(num_threads) if(num_threads > 1)
  {
#endif
    // get a thread-safe fdata struct (one jpeg struct per thread etc):
    dt_imageio_module_data_t *fdata = mformat->get_params(mformat, &size);
    fdata->max_width  = dt_conf_get_int ("plugins/lighttable/export/width");
    fdata->max_height = dt_conf_get_int ("plugins/lighttable/export/height");
    fdata->max_width = (w!=0 && fdata->max_width >w)?w:fdata->max_width;
    fdata->max_height = (h!=0 && fdata->max_height >h)?h:fdata->max_height;
    int num = 0;
    // Invariant: the tagid for 'darktable|changed' will not change while this function runs. Is this a sensible assumption?
    guint tagid = 0;
    dt_tag_new("darktable|changed",&tagid);

    while(t && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED)
    {
#ifdef _OPENMP
      #pragma omp critical
#endif
      {
        if(!t) 
          imgid = 0; 
        else
        {
          imgid = (long int)t->data;
          t = g_list_delete_link(t, t);
          num = total - g_list_length(t);
        }
      }
      // remove 'changed' tag from image
      dt_tag_detach(tagid, imgid);
      // check if image still exists:
      char imgfilename[1024];
      dt_image_t *image = dt_image_cache_get(imgid, 'r');
      if(image)
      {
  dt_image_full_path(image->id, imgfilename, 1024);
  if(!g_file_test(imgfilename, G_FILE_TEST_IS_REGULAR))
  {
    dt_control_log(_("image `%s' is currently unavailable"), image->filename);
    fprintf(stderr, _("image `%s' is currently unavailable"), imgfilename);
    // dt_image_remove(imgid);
    dt_image_cache_release(image, 'r');
  }
  else
  {
    dt_image_cache_release(image, 'r');
    mstorage->store(sdata, imgid, mformat, fdata, num, total);
  }
      }
#ifdef _OPENMP
      #pragma omp critical
#endif
      {
        fraction+=1.0/total;
	dt_control_backgroundjobs_progress(control, jid, fraction);
      }
    }
#ifdef _OPENMP
    #pragma omp barrier
    #pragma omp master
#endif
    {
      dt_control_backgroundjobs_destroy(control, jid);
      if(mstorage->finalize_store) mstorage->finalize_store(mstorage, sdata);
      mstorage->free_params(mstorage, sdata);
    }
    // all threads free their fdata
    mformat->free_params (mformat, fdata);
#ifdef _OPENMP
  }
#endif
  return 0;
}

void dt_control_export_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "export");
  job->execute = &dt_control_export_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_init(t);
}

void dt_control_export()
{
  dt_job_t j;
  dt_control_export_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

void dt_control_start_indexer() {
  dt_job_t j;
  dt_control_indexer_job_init(&j);
  dt_control_add_background_job(darktable.control, &j, 10);
}


// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
