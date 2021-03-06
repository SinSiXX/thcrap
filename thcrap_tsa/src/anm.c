/**
  * Touhou Community Reliant Automatic Patcher
  * Team Shanghai Alice support plugin
  *
  * ----
  *
  * On-the-fly ANM patcher.
  *
  * Portions adapted from xarnonymous' Touhou Toolkit
  * http://code.google.com/p/thtk/
  */

#include <thcrap.h>
#include <png.h>
#include "png_ex.h"
#include "thcrap_tsa.h"
#include "anm.h"

/// JSON-based structure data access
/// --------------------------------
int struct_get(void *dest, size_t dest_size, void *src, json_t *spec)
{
	if(!dest || !dest_size || !src || !spec) {
		return -1;
	}
	{
		size_t offset = json_object_get_hex(spec, "offset");
		size_t size = json_object_get_hex(spec, "size");
		// Default to architecture word size
		if(!size) {
			size = sizeof(size_t);
		};
		if(size > dest_size) {
			return -2;
		}
		ZeroMemory(dest, dest_size);
		memcpy(dest, (char*)src + offset, size);
	}
	return 0;
}

#define STRUCT_GET(val, src, spec_obj) \
	struct_get(&(val), sizeof(val), src, json_object_get(spec_obj, #val))
/// --------------------------------

/// Formats
/// -------
unsigned int format_Bpp(format_t format)
{
	switch(format) {
		case FORMAT_BGRA8888:
			return 4;
		case FORMAT_ARGB4444:
		case FORMAT_RGB565:
			return 2;
		case FORMAT_GRAY8:
			return 1;
		default:
			log_printf("unknown format: %u\n", format);
			return 0;
	}
}

png_uint_32 format_png_equiv(format_t format)
{
	switch(format) {
		case FORMAT_BGRA8888:
		case FORMAT_ARGB4444:
		case FORMAT_RGB565:
			return PNG_FORMAT_BGRA;
		case FORMAT_GRAY8:
			return PNG_FORMAT_GRAY;
		default:
			log_printf("unknown format: %u\n", format);
			return PNG_FORMAT_INVALID;
	}
}

png_byte format_alpha_max(format_t format)
{
	switch(format) {
		case FORMAT_BGRA8888:
			return 0xff;
		case FORMAT_ARGB4444:
			return 0xf;
		default:
			return 0;
	}
}

size_t format_alpha_sum(png_bytep data, unsigned int pixels, format_t format)
{
	size_t ret = 0;
	unsigned int i;
	if(format == FORMAT_BGRA8888) {
		for(i = 0; i < pixels; ++i, data += 4) {
			ret += data[3];
		}
	} else if(format == FORMAT_ARGB4444) {
		for(i = 0; i < pixels; ++i, data += 2) {
			ret += (data[1] & 0xf0) >> 4;
		}
	}
	return ret;
}

void format_from_bgra(png_bytep data, unsigned int pixels, format_t format)
{
	unsigned int i;
	png_bytep in = data;

	if(format == FORMAT_ARGB4444) {
		png_bytep out = data;
		for(i = 0; i < pixels; ++i, in += 4, out += 2) {
			// I don't see the point in doing any "rounding" here. Let's rather focus on
			// writing understandable code independent of endianness assumptions.
			const unsigned char b = in[0] >> 4;
			const unsigned char g = in[1] >> 4;
			const unsigned char r = in[2] >> 4;
			const unsigned char a = in[3] >> 4;
			// Yes, we start with the second byte. "Little-endian ARGB", mind you.
			out[1] = (a << 4) | r;
			out[0] = (g << 4) | b;
		}
	} else if(format == FORMAT_RGB565) {
		png_uint_16p out16 = (png_uint_16p)data;
		for(i = 0; i < pixels; ++i, in += 4, ++out16) {
			const unsigned char b = in[0] >> 3;
			const unsigned char g = in[1] >> 2;
			const unsigned char r = in[2] >> 3;

			out16[0] = (r << 11) | (g << 5) | b;
		}
	}
	// FORMAT_GRAY8 is fully handled by libpng
}

void format_copy(png_bytep dst, png_bytep rep, unsigned int pixels, format_t format)
{
	memcpy(dst, rep, pixels * format_Bpp(format));
}

void format_blend(png_bytep dst, png_bytep rep, unsigned int pixels, format_t format)
{
	// Alpha values are added and clamped to the format's maximum. This avoids a
	// flaw in the blending algorithm, which may decrease the alpha value even if
	// both target and replacement pixels are fully opaque.
	// (This also seems to be what the default composition mode in GIMP does.)
	unsigned int i;
	if(format == FORMAT_BGRA8888) {
		for(i = 0; i < pixels; ++i, dst += 4, rep += 4) {
			const int new_alpha = dst[3] + rep[3];
			const int dst_alpha = 0xff - rep[3];

			dst[0] = (dst[0] * dst_alpha + rep[0] * rep[3]) >> 8;
			dst[1] = (dst[1] * dst_alpha + rep[1] * rep[3]) >> 8;
			dst[2] = (dst[2] * dst_alpha + rep[2] * rep[3]) >> 8;
			dst[3] = min(new_alpha, 0xff);
		}
	} else if(format == FORMAT_ARGB4444) {
		for(i = 0; i < pixels; ++i, dst += 2, rep += 2) {
			const unsigned char rep_a = (rep[1] & 0xf0) >> 4;
			const unsigned char rep_r = (rep[1] & 0x0f) >> 0;
			const unsigned char rep_g = (rep[0] & 0xf0) >> 4;
			const unsigned char rep_b = (rep[0] & 0x0f) >> 0;
			const unsigned char dst_a = (dst[1] & 0xf0) >> 4;
			const unsigned char dst_r = (dst[1] & 0x0f) >> 0;
			const unsigned char dst_g = (dst[0] & 0xf0) >> 4;
			const unsigned char dst_b = (dst[0] & 0x0f) >> 0;
			const int new_alpha = dst_a + rep_a;
			const int dst_alpha = 0xf - rep_a;

			dst[1] =
				(min(new_alpha, 0xf) << 4) |
				((dst_r * dst_alpha + rep_r * rep_a) >> 4);
			dst[0] =
				(dst_g * dst_alpha + rep_g * rep_a & 0xf0) |
				((dst_b * dst_alpha + rep_b * rep_a) >> 4);
		}
	} else {
		// Other formats have no alpha channel, so we can just do...
		format_copy(dst, rep, pixels, format);
	}
}
/// -------

/// Sprite-level patching
/// ---------------------
int sprite_patch_set(
	sprite_patch_t *sp,
	const anm_entry_t *entry,
	const sprite_local_t *sprite,
	const png_image_exp image
)
{
	if(!sp || !entry || !entry->thtx || !sprite || !image || !image->buf) {
		return -1;
	}
	ZeroMemory(sp, sizeof(*sp));

	// Note that we don't use the PNG_IMAGE_* macros here - the actual bit depth
	// after format_from_bgra() may no longer be equal to the one in the PNG header.
	sp->format = entry->thtx->format;
	sp->bpp = format_Bpp(sp->format);

	sp->dst_x = sprite->x;
	sp->dst_y = sprite->y;

	sp->rep_x = entry->x + sp->dst_x;
	sp->rep_y = entry->y + sp->dst_y;

	if(
		sp->dst_x >= entry->thtx->w || sp->dst_y >= entry->thtx->h ||
		sp->rep_x >= image->img.width || sp->rep_y >= image->img.height
	) {
		return 2;
	}

	sp->rep_stride = image->img.width * sp->bpp;
	sp->dst_stride = entry->thtx->w * sp->bpp;

	sp->copy_w = min(sprite->w, (image->img.width - sp->rep_x));
	sp->copy_h = min(sprite->h, (image->img.height - sp->rep_y));

	sp->dst_buf = entry->thtx->data + (sp->dst_y * sp->dst_stride) + (sp->dst_x * sp->bpp);
	sp->rep_buf = image->buf + (sp->rep_y * sp->rep_stride) + (sp->rep_x * sp->bpp);
	return 0;
}

sprite_alpha_t sprite_alpha_analyze(
	const png_bytep buf,
	const format_t format,
	const size_t stride,
	const png_uint_32 w,
	const png_uint_32 h
)
{
	const size_t opaque_sum = format_alpha_max(format) * w;
	if(!buf) {
		return SPRITE_ALPHA_EMPTY;
	} else if(!opaque_sum) {
		return SPRITE_ALPHA_OPAQUE;
	} else {
		sprite_alpha_t ret = SPRITE_ALPHA_FULL;
		png_uint_32 row;
		png_bytep p = buf;
		for(row = 0; row < h; row++) {
			size_t sum = format_alpha_sum(p, w, format);
			if(sum == 0x00 && ret != SPRITE_ALPHA_OPAQUE) {
				ret = SPRITE_ALPHA_EMPTY;
			} else if(sum == opaque_sum && ret != SPRITE_ALPHA_EMPTY) {
				ret = SPRITE_ALPHA_OPAQUE;
			} else {
				return SPRITE_ALPHA_FULL;
			}
			p += stride;
		}
		return ret;
	}
}

sprite_alpha_t sprite_alpha_analyze_rep(const sprite_patch_t *sp)
{
	if(sp) {
		return sprite_alpha_analyze(sp->rep_buf, sp->format, sp->rep_stride, sp->copy_w, sp->copy_h);
	} else {
		return SPRITE_ALPHA_EMPTY;
	}
}

sprite_alpha_t sprite_alpha_analyze_dst(const sprite_patch_t *sp)
{
	if(sp) {
		return sprite_alpha_analyze(sp->dst_buf, sp->format, sp->dst_stride, sp->copy_w, sp->copy_h);
	} else {
		return SPRITE_ALPHA_EMPTY;
	}
}

int sprite_blit(const sprite_patch_t *sp, const BlitFunc_t func)
{
	if(sp && func) {
		png_uint_32 row;
		png_bytep dst_p = sp->dst_buf;
		png_bytep rep_p = sp->rep_buf;
		for(row = 0; row < sp->copy_h; row++) {
			func(dst_p, rep_p, sp->copy_w, sp->format);
			dst_p += sp->dst_stride;
			rep_p += sp->rep_stride;
		}
		return 0;
	}
	return -1;
}

sprite_alpha_t sprite_patch(const sprite_patch_t *sp)
{
	sprite_alpha_t rep_alpha = sprite_alpha_analyze_rep(sp);
	if(rep_alpha != SPRITE_ALPHA_EMPTY) {
		BlitFunc_t func = NULL;
		sprite_alpha_t dst_alpha = sprite_alpha_analyze_dst(sp);
		if(dst_alpha == SPRITE_ALPHA_OPAQUE) {
			func = format_blend;
		} else {
			func = format_copy;
		}
		sprite_blit(sp, func);
	}
	return rep_alpha;
}
/// ---------------------

/// ANM structure
/// -------------
sprite_local_t *sprite_split_new(anm_entry_t *entry)
{
	sprite_local_t *sprites_new = (sprite_local_t*)realloc(
		entry->sprites, (entry->sprite_num + 1) * sizeof(sprite_local_t)
	);
	if(!sprites_new) {
		return NULL;
	}
	entry->sprites = sprites_new;
	return &sprites_new[entry->sprite_num++];
}

int sprite_split_x(anm_entry_t *entry, sprite_local_t *sprite)
{
	if(entry && entry->thtx && entry->thtx->w && sprite) {
		png_uint_32 split_w = sprite->x + sprite->w;
		if(split_w > entry->thtx->w) {
			sprite_local_t *sprite_new = sprite_split_new(entry);
			if(!sprite_new) {
				return 1;
			}
			sprite_new->x = 0;
			sprite_new->y = sprite->y;
			sprite_new->w = min(split_w - entry->thtx->w, sprite->x);
			sprite_new->h = sprite->h;
			return sprite_split_y(entry, sprite_new);
		}
		return 0;
	}
	return -1;
}

int sprite_split_y(anm_entry_t *entry, sprite_local_t *sprite)
{
	if(entry && entry->thtx && entry->thtx->h && sprite) {
		png_uint_32 split_h = sprite->y + sprite->h;
		if(split_h > entry->thtx->h) {
			sprite_local_t *sprite_new = sprite_split_new(entry);
			if(!sprite_new) {
				return 1;
			}
			sprite_new->x = sprite->x;
			sprite_new->y = 0;
			sprite_new->w = sprite->w;
			sprite_new->h = min(split_h - entry->thtx->h, sprite->h);
			return sprite_split_x(entry, sprite_new);
		}
		return 0;
	}
	return -1;
}

int anm_entry_init(anm_entry_t *entry, BYTE *in, json_t *format)
{
	size_t x;
	size_t y;
	size_t nameoffset;
	size_t thtxoffset;
	size_t hasdata;
	size_t nextoffset;
	size_t sprites;
	size_t headersize;

	if(!entry || !in || !json_is_object(format)) {
		return -1;
	}

	anm_entry_clear(entry);
	headersize = json_object_get_hex(format, "headersize");

	if(
		STRUCT_GET(x, in, format) ||
		STRUCT_GET(y, in, format) ||
		STRUCT_GET(nameoffset, in, format) ||
		STRUCT_GET(thtxoffset, in, format) ||
		STRUCT_GET(hasdata, in, format) ||
		STRUCT_GET(nextoffset, in, format) ||
		STRUCT_GET(sprites, in, format)
	) {
		return 1;
	}
	entry->x = x;
	entry->y = y;
	entry->hasbitmap = hasdata;
	entry->nextoffset = nextoffset;
	entry->sprite_num = sprites;
	entry->name = (const char*)(nameoffset + (size_t)in);
	entry->thtx = (thtx_header_t*)(thtxoffset + (size_t)in);

	// Prepare sprite pointers if we have a header size.
	// Otherwise, we fall back to basic patching later.
	if(headersize) {
		// This will change with splits being appended...
		size_t sprite_orig_num = entry->sprite_num;
		size_t i;
		DWORD *sprite_in = (DWORD*)(in + headersize);
		entry->sprites = malloc(sizeof(sprite_local_t) * sprite_orig_num);
		for(i = 0; i < sprite_orig_num; i++, sprite_in++) {
			const sprite_t *s_orig = (const sprite_t*)(in + *sprite_in);
			sprite_local_t *s_local = &entry->sprites[i];

			s_local->x = (png_uint_32)s_orig->x;
			s_local->y = (png_uint_32)s_orig->y;
			s_local->w = (png_uint_32)s_orig->w;
			s_local->h = (png_uint_32)s_orig->h;
			sprite_split_x(entry, s_local);
			sprite_split_y(entry, s_local);
		}
	}
	return 0;
}

void anm_entry_clear(anm_entry_t *entry)
{
	if(entry) {
		SAFE_FREE(entry->sprites);
		ZeroMemory(entry, sizeof(*entry));
	}
}
/// -------------

int patch_png_load_for_thtx(png_image_exp image, const json_t *patch_info, const char *fn, thtx_header_t *thtx)
{
	void *file_buffer = NULL;
	size_t file_size;

	if(!image || !thtx) {
		return -1;
	}

	SAFE_FREE(image->buf);
	png_image_free(&image->img);
	ZeroMemory(&image->img, sizeof(png_image));
	image->img.version = PNG_IMAGE_VERSION;

	if(strncmp(thtx->magic, "THTX", sizeof(thtx->magic))) {
		return 1;
	}

	file_buffer = patch_file_load(patch_info, fn, &file_size);
	if(!file_buffer) {
		return 2;
	}

	if(png_image_begin_read_from_memory(&image->img, file_buffer, file_size)) {
		image->img.format = format_png_equiv(thtx->format);
		if(image->img.format != PNG_FORMAT_INVALID) {
			size_t png_size = PNG_IMAGE_SIZE(image->img);
			image->buf = (png_bytep)malloc(png_size);

			if(image->buf) {
				png_image_finish_read(&image->img, 0, image->buf, 0, NULL);
			}
		}
	}
	SAFE_FREE(file_buffer);
	if(image->buf) {
		format_from_bgra(image->buf, image->img.width * image->img.height, thtx->format);
	}
	return !image->buf;
}

// Patches an [image] prepared by <png_load_for_thtx> into [entry].
// Patching will be performed on sprite level if the <sprites> and
// <sprite_num> members of [entry] are valid.
// [png] is assumed to have the same bit depth as the texture in [entry].
int patch_thtx(anm_entry_t *entry, png_image_exp image)
{
	if(!entry || !entry->thtx || !image || !image->buf) {
		return -1;
	}
	if(entry->sprites && entry->sprite_num > 1) {
		size_t i;
		for(i = 0; i < entry->sprite_num; i++) {
			sprite_patch_t sp;
			if(!sprite_patch_set(&sp, entry, &entry->sprites[i], image)) {
				sprite_patch(&sp);
			}
		}
	} else {
		// Construct a fake sprite covering the entire texture
		sprite_local_t sprite = {0};
		sprite_patch_t sp = {0};

		sprite.w = entry->thtx->w;
		sprite.h = entry->thtx->h;
		if(!sprite_patch_set(&sp, entry, &sprite, image)) {
			return sprite_patch(&sp);
		}
	}
	return 0;
}

// Helper function for stack_game_png_apply.
int patch_png_apply(anm_entry_t *entry, const json_t *patch_info, const char *fn)
{
	int ret = -1;
	if(entry && patch_info && fn) {
		png_image_ex png = {0};
		ret = patch_png_load_for_thtx(&png, patch_info, fn, entry->thtx);
		if(!ret) {
			patch_thtx(entry, &png);
			patch_print_fn(patch_info, fn);
		}
		SAFE_FREE(png.buf);
	}
	return ret;
}

int stack_game_png_apply(anm_entry_t *entry)
{
	int ret = -1;
	if(entry && entry->hasbitmap && entry->thtx && entry->name) {
		stack_chain_iterate_t sci = {0};
		json_t *chain = resolve_chain_game(entry->name);
		ret = 0;
		if(json_array_size(chain)) {
			log_printf("(PNG) Resolving %s... ", json_array_get_string(chain, 0));
		}
		while(stack_chain_iterate(&sci, chain, SCI_FORWARDS)) {
			if(!patch_png_apply(entry, sci.patch_info, sci.fn)) {
				ret = 1;
			}
		}
		log_printf(ret ? "\n" : "not found\n");
		json_decref(chain);
	}
	return ret;
}

int patch_anm(BYTE *file_inout, size_t size_out, size_t size_in, json_t *patch)
{
	json_t *format = specs_get("anm");
	json_t *dat_dump = json_object_get(runconfig_get(), "dat_dump");
	size_t headersize = json_object_get_hex(format, "headersize");

	// Some ANMs reference the same file name multiple times in a row
	const char *name_prev = NULL;

	png_image_ex png = {0};
	png_image_ex bounds = {0};

	BYTE *anm_entry_out = file_inout;

	if(!format) {
		return 1;
	}

	log_printf("---- ANM ----\n");

	if(!headersize) {
		log_printf("(no ANM header size given, sprite-local patching disabled)\n");
	}

	while(anm_entry_out && anm_entry_out < file_inout + size_in) {
		anm_entry_t entry = {0};
		if(anm_entry_init(&entry, anm_entry_out, format)) {
			log_printf("Corrupt ANM file or format definition, aborting ...\n");
			break;
		}
		if(entry.hasbitmap && entry.thtx) {
			if(!name_prev || strcmp(entry.name, name_prev)) {
				if(!json_is_false(dat_dump)) {
					bounds_store(name_prev, &bounds);
					bounds_init(&bounds, entry.thtx, entry.name);
				}
				name_prev = entry.name;
			}
			png_image_resize(&bounds, entry.x + entry.thtx->w, entry.y + entry.thtx->h);
			if(entry.sprites) {
				size_t i;
				for(i = 0; i < entry.sprite_num; i++) {
					bounds_draw_rect(&bounds, entry.x, entry.y, &entry.sprites[i]);
				}
			}
			// Do the patching
			stack_game_png_apply(&entry);
		}
		if(!entry.nextoffset) {
			bounds_store(name_prev, &bounds);
			anm_entry_out = NULL;
		}
		anm_entry_out += entry.nextoffset;
		anm_entry_clear(&entry);
	}
	png_image_clear(&bounds);
	png_image_clear(&png);
	log_printf("-------------\n");
	return 0;
}
