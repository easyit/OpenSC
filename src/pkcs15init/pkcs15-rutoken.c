/*
 * ruToken specific operation for PKCS15 initialization
 *
 * Copyright (C) 2007  Pavel Mironchik <rutoken@rutoken.ru>
 * Copyright (C) 2007  Eugene Hermann <rutoken@rutoken.ru> 
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
 
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <opensc/opensc.h>
#include <opensc/cardctl.h>
#include <opensc/log.h>
#include <opensc/pkcs15.h>
#include <opensc/rutoken.h>
#include "pkcs15-init.h"
#include "profile.h"

#define MAX_ID 255

static const sc_SecAttrV2_t pr_sec_attr = {0x43, 1, 1, 0, 0, 0, 0, 1, 2, 2, 0, 0, 0, 0, 2};
static const sc_SecAttrV2_t pb_sec_attr = {0x42, 0, 1, 0, 0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 2};
static const sc_SecAttrV2_t wn_sec_attr = {0x43, 1, 1, 0, 0, 0, 0,-1, 2, 2, 0, 0, 0, 0, 0};
static const sc_SecAttrV2_t df_sec_attr = {0x43, 1, 1, 0, 0, 0, 0, 1, 2, 2, 0, 0, 0, 0, 2};
static const sc_SecAttrV2_t ef_sec_attr = {0x42, 0, 1, 0, 0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 2};
static const sc_SecAttrV2_t p2_sec_attr = {0x43, 1, 1, 0, 0, 0, 0,-1, 1, 2, 0, 0, 0, 0, 0};
static const sc_SecAttrV2_t p1_sec_attr = {0x43, 1, 1, 0, 0, 0, 0,-1, 1, 1, 0, 0, 0, 0, 0};

enum DF_IDs
{
	PrKDFid = 0x1001,
	PuKDFid = 0x1002,
	CDFid   = 0x1003,
	DODFid  = 0x1004,
	AODFid  = 0xFFFF
};

#define PrKDF_name      "PKCS15-PrKDF"
#define PuKDF_name      "PKCS15-PuKDF"
#define CDF_name        "PKCS15-CDF"
#define DODF_name       "PKCS15-DODF"
#define AODF_name       "PKCS15-AODF"
#define ODF_name        "PKCS15-ODF"

static const struct
{
	char const*  name;
	unsigned int dir;
	unsigned int type;
} arr_def_df[] =
		{
			{ PrKDF_name, PrKDFid, SC_PKCS15_PRKDF },
			{ PuKDF_name, PuKDFid, SC_PKCS15_PUKDF },
			{ CDF_name,   CDFid,   SC_PKCS15_CDF   },
			{ DODF_name,  DODFid,  SC_PKCS15_DODF  },
			{ AODF_name,  AODFid,  SC_PKCS15_AODF  }
		};

/*
 * Create/override new EF.
 */
static int rutoken_create_file(sc_card_t *card, sc_path_t *path, sc_file_t *ef)
{
	int ret = SC_SUCCESS;
	sc_path_t path_dir;

	SC_FUNC_CALLED(card->ctx, 1);

	if (path)
	{
		sc_ctx_suppress_errors_on(card->ctx);
		ret = card->ops->select_file(card, path, NULL);
		sc_ctx_suppress_errors_off(card->ctx);
		if (ret == SC_SUCCESS)
		{
			sc_path_t del_path;
			del_path.len = 2;
			del_path.type = SC_PATH_TYPE_FILE_ID;
			del_path.value[0] = (u8)(ef->id / 256);
			del_path.value[1] = (u8)(ef->id % 256);
			if (card->ops->select_file(card, &del_path, NULL) == SC_SUCCESS)
				ret = card->ops->delete_file(card, &del_path);
		}
		path_dir = *path;
		path_dir.len -= 2;
		ret = card->ops->select_file(card, &path_dir, NULL);
	}
	if (ret == SC_SUCCESS)
	{
		ret = card->ops->create_file(card, ef);
	}
	return ret;
}

/*
 * Create a DF
 */
static int
rutoken_create_dir(sc_profile_t *profile, sc_card_t *card, sc_file_t *df)
{
	int ret;
	sc_file_t *file = NULL;

	if (!card || !card->ctx || !df)
		return SC_ERROR_INVALID_ARGUMENTS;

	SC_FUNC_CALLED(card->ctx, 1);

	ret = card->ops->select_file(card, &df->path, &file);
	if (ret == SC_ERROR_FILE_NOT_FOUND)
		ret = card->ops->create_file(card, df);
	else if(file && file->type != SC_FILE_TYPE_DF)
		ret = SC_ERROR_WRONG_CARD;

	if(file)
		sc_file_free(file);
	return ret;
}

static int get_dfpath(sc_profile_t *profile, unsigned int df_type,
			sc_path_t *out_path)
{
	static const int DFid[SC_PKCS15_DF_TYPE_COUNT] =
			{ PrKDFid, PuKDFid, 0, 0, CDFid, 0, 0, DODFid };
	/* assert(0 == SC_PKCS15_PRKDF);
	 * assert(1 == SC_PKCS15_PUKDF);
	 * assert(4 == SC_PKCS15_CDF);
	 * assert(7 == SC_PKCS15_DODF);
	 */

	if (df_type >= SC_PKCS15_DF_TYPE_COUNT || !profile
			|| !profile->df || !profile->df[df_type]
	)
		return -1;

	*out_path = profile->df[df_type]->path;
	out_path->len -= 2;
	sc_append_file_id(out_path, DFid[df_type]);
	return 0;
}

/*
 * Select a key reference
 */
static int
rutoken_select_key_reference(sc_profile_t *profile, sc_card_t *card,
			sc_pkcs15_prkey_info_t *key_info)
{
	if (!profile || !card || !card->ctx || !key_info)
		return SC_ERROR_INVALID_ARGUMENTS;

	SC_FUNC_CALLED(card->ctx, 1);

	if (get_dfpath(profile, SC_PKCS15_PRKDF, &key_info->path) < 0)
	{
		sc_debug(card->ctx, "Call error get_dfpath\n");
		return SC_ERROR_INTERNAL;
	}
	sc_append_file_id(&key_info->path, key_info->key_reference);

	return key_info->key_reference >= 0 && key_info->key_reference <= MAX_ID ? 
			SC_SUCCESS : SC_ERROR_TOO_MANY_OBJECTS;
}

/*
 * Create a private key object.
 * This is a no-op.
 */
static int
rutoken_create_key(sc_profile_t *profile, sc_card_t *card,
			sc_pkcs15_object_t *obj)
{
	if (!profile || !card || !card->ctx || !obj)
		return SC_ERROR_INVALID_ARGUMENTS;
	SC_FUNC_CALLED(card->ctx, 1);
	return SC_SUCCESS;
}

/*
 * Create private key files
 */
static int rutoken_create_prkeyfile(sc_card_t *card,
			sc_pkcs15_prkey_info_t *key_info, size_t prsize)
{
	sc_path_t path;
	sc_file_t *file;
	int ret;

	SC_FUNC_CALLED(card->ctx, 1);

	file = sc_file_new();
	if (file)
	{
		/* create key file */
		path = key_info->path;
		file->type = SC_FILE_TYPE_WORKING_EF;
		file->id = key_info->key_reference;
		file->size = prsize;
		sc_file_set_sec_attr(file, (u8*)&pr_sec_attr, SEC_ATTR_SIZE);
		ret = rutoken_create_file(card, &path, file);
		if (file)
			sc_file_free(file);
	}
	else
		ret = SC_ERROR_OUT_OF_MEMORY;
	return ret;
}

/* 
 * Store a private key object.
 */
static int
rutoken_store_key(sc_profile_t *profile, sc_card_t *card,
			sc_pkcs15_object_t *obj,
			sc_pkcs15_prkey_t *key)
{
	sc_pkcs15_prkey_info_t *key_info;
	const int nKeyBufSize = 2048;
	u8 *prkeybuf = NULL;
	size_t prsize;
	int ret;

	if (!profile || !profile->ops || !card || !card->ctx
			|| !obj || !obj->data || !key
			|| !profile->ops->encode_private_key
	)
		return SC_ERROR_INVALID_ARGUMENTS;

	SC_FUNC_CALLED(card->ctx, 1);

	if (obj->type != SC_PKCS15_TYPE_PRKEY_RSA)
		return SC_ERROR_NOT_SUPPORTED;

	key_info = (sc_pkcs15_prkey_info_t *) obj->data;
	prkeybuf = calloc(nKeyBufSize, 1);
	if(!prkeybuf)
		return SC_ERROR_OUT_OF_MEMORY;

	/*
	 * encode private key 
	 * create key file 
	 * write a key
	 */
	prsize = nKeyBufSize;
	ret = profile->ops->encode_private_key(profile, card, &key->u.rsa, prkeybuf, &prsize, 0);
	if (ret == 0)
	{
		ret = rutoken_create_prkeyfile(card, key_info, prsize);
		if (ret == 0)
		{
			ret = sc_update_binary(card, 0, prkeybuf, prsize, 0);
			if (ret < 0  ||  (size_t)ret != prsize)
				sc_debug(card->ctx, "ret=%i (%u)\n", ret, prsize);
		}
		memset(prkeybuf, 0, prsize);
	}
	free(prkeybuf);
	return ret;
}

/*
 * Encode private key
 */
static int
rutoken_encode_private_key(sc_profile_t *profile, sc_card_t *card,
			struct sc_pkcs15_prkey_rsa *rsa,
			u8 *key, size_t *keysize, int key_ref)
{
	int r;

	if (!card || !card->ctx || !rsa || !key || !keysize)
		return SC_ERROR_INVALID_ARGUMENTS;

	SC_FUNC_CALLED(card->ctx, 1);
	r = sc_rutoken_get_bin_from_prkey(rsa, key, keysize);
	sc_debug(card->ctx, "sc_rutoken_get_bin_from_prkey returned %i\n", r);
	return r;
}

static int rutoken_id_in(int id, const u8 *buf, int buflen)
{
	int i;
	for (i = 0; i*2 < buflen; i++)
		if (id == (int)buf[i*2] * 0x100 + buf[i*2 + 1]) return 1;
	return 0;
}

static int rutoken_find_id(sc_card_t *card, const sc_path_t *path)
{
	int ret = SC_SUCCESS;
	sc_file_t *file = NULL;
	u8 *files = malloc(2048);
	if (!files) return SC_ERROR_OUT_OF_MEMORY;
	if(path)
	{
		if((ret = card->ops->select_file(card, path, &file)) == SC_SUCCESS)
			ret = file->type == SC_FILE_TYPE_DF ? SC_SUCCESS : SC_ERROR_NOT_ALLOWED;
	}
	if(ret == SC_SUCCESS)
	{
		ret = card->ops->list_files(card, files, 2048);
		if(ret >= 0)
		{
			int i;
			for (i = 0; i < MAX_ID; i++)
				if(!rutoken_id_in(i, files, ret)) {ret = i; break;}
		}
	}
	free(files);
	if(file) sc_file_free(file);
	return ret;
}

/*
 * Create a file based on a PKCS15_TYPE_xxx
 */
static int
rutoken_new_file(struct sc_profile *profile, struct sc_card *card,
			unsigned int type, unsigned int idx, struct sc_file **file)
{
	int ret = SC_SUCCESS, id, ret_s;
	sc_path_t path;
	u8 const *sec_attr;

	if (!profile || !file || *file != NULL
			|| !card || !card->ctx || !card->ops
			|| !card->ops->delete_file || !card->ops->select_file
			|| !card->ops->list_files /* for call rutoken_find_id */
	)
		return SC_ERROR_INVALID_ARGUMENTS;

	SC_FUNC_CALLED(card->ctx, 1);

	switch (type & SC_PKCS15_TYPE_CLASS_MASK)
	{
		case SC_PKCS15_TYPE_CERT:
			ret = get_dfpath(profile, SC_PKCS15_CDF, &path);
			sec_attr = pb_sec_attr;
			break;
		case SC_PKCS15_TYPE_PUBKEY:
			ret = get_dfpath(profile, SC_PKCS15_PUKDF, &path);
			sec_attr = pb_sec_attr;
			break;
		case SC_PKCS15_TYPE_DATA_OBJECT:
			ret = get_dfpath(profile, SC_PKCS15_DODF, &path);
			sec_attr = pr_sec_attr;
			break;
		case SC_PKCS15_TYPE_PRKEY_RSA:
		default:
			ret = SC_ERROR_NOT_SUPPORTED;
	}
	/* find first unlished file id */
	if (ret == SC_SUCCESS)
	{
		id = rutoken_find_id(card, &path);
		if (id < 0)
		{
			sc_debug(card->ctx, "Error find id (%i)\n", id);
			ret = SC_ERROR_TOO_MANY_OBJECTS;
		}
	}
	if(ret == SC_SUCCESS)
	{
		sc_debug(card->ctx, "new id %i\n", id);
		*file = sc_file_new();
		if (!*file)
			ret = SC_ERROR_OUT_OF_MEMORY;
		{
			(*file)->size = 0;
			(*file)->id = id;
			sc_append_file_id(&path, (*file)->id);
			(*file)->path = path;
			sc_file_set_sec_attr(*file, sec_attr, SEC_ATTR_SIZE);
			(*file)->type = SC_FILE_TYPE_WORKING_EF;
			/*  If target file exist than remove it */
			sc_ctx_suppress_errors_on(card->ctx);
			ret_s = card->ops->select_file(card, &(*file)->path, NULL);
			sc_ctx_suppress_errors_off(card->ctx);
			if (ret_s == SC_SUCCESS)
			{
				sc_path_t del_path;
				del_path.len = 0;
				del_path.type = SC_PATH_TYPE_FILE_ID;
				card->ops->delete_file(card, &del_path);
			}
		}
	}
	return ret;
}

/*
 * Initialization routine
 */

static const struct
{
	u8                     id, options, flags, try, pass[8];
	sc_SecAttrV2_t const*  p_sattr;
} do_pins[] =
		{
			{ SC_RUTOKEN_DEF_ID_GCHV_USER, SC_RUTOKEN_OPTIONS_GACCESS_USER,
			  SC_RUTOKEN_FLAGS_COMPACT_DO, 0xFF,
			  { '1', '2', '3', '4', '5', '6', '7', '8' }, &p2_sec_attr
			},
			{ SC_RUTOKEN_DEF_ID_GCHV_ADMIN, SC_RUTOKEN_OPTIONS_GACCESS_ADMIN,
			  SC_RUTOKEN_FLAGS_COMPACT_DO, 0xFF,
			  { '8', '7', '6', '5', '4', '3', '2', '1' }, &p1_sec_attr
			}
		};

static int create_pins(sc_card_t *card)
{
	sc_DO_V2_t param_do;
	size_t i;
	int r = SC_SUCCESS;

	for (i = 0; i < sizeof(do_pins)/sizeof(do_pins[0]); ++i)
	{
		memset(&param_do, 0, sizeof(param_do));
		param_do.HDR.OTID.byObjectType  = SC_RUTOKEN_TYPE_CHV;
		param_do.HDR.OTID.byObjectID    = do_pins[i].id;
		param_do.HDR.OP.byObjectOptions = do_pins[i].options;
		param_do.HDR.OP.byObjectFlags   = do_pins[i].flags;
		param_do.HDR.OP.byObjectTry     = do_pins[i].try;
		param_do.HDR.wDOBodyLen = sizeof(do_pins[i].pass);
		/* assert(do_pins[i].p_sattr != NULL); */
		/* assert(sizeof(*param_do.HDR.SA_V2)) */
		/* assert(sizeof(param_do.HDR.SA_V2) == sizeof(*do_pins[i].p_sattr)); */
		memcpy(param_do.HDR.SA_V2, *do_pins[i].p_sattr, 
				sizeof(*do_pins[i].p_sattr));
		/* assert(do_pins[i].pass); */
		/* assert(sizeof(*param_do.abyDOBody)) */
		/* assert(sizeof(param_do.abyDOBody) >= sizeof(do_pins[i].pass)); */
		memcpy(param_do.abyDOBody, do_pins[i].pass, sizeof(do_pins[i].pass));

		r = sc_card_ctl(card, SC_CARDCTL_RUTOKEN_CREATE_DO, &param_do);
		if (r != SC_SUCCESS) break;
	}
	return r;
}

static int create_typical_fs(sc_card_t *card)
{
	sc_file_t *df;
	int r;

	df = sc_file_new();
	if (!df)
		return SC_ERROR_OUT_OF_MEMORY;
	df->type = SC_FILE_TYPE_DF;
	do
	{
		r = sc_file_set_sec_attr(df, wn_sec_attr, SEC_ATTR_SIZE);
		if (r != SC_SUCCESS) break;

		/* Create MF  3F00 */
		df->id = 0x3F00;
		sc_format_path("3F00", &df->path);
		r = card->ops->create_file(card, df);
		if (r != SC_SUCCESS) break;

		/* Create     3F00/0000 */
		df->id = 0x0000;
		sc_append_file_id(&df->path, df->id);
		r = card->ops->create_file(card, df);
		if (r != SC_SUCCESS) break;

		/* Create     3F00/0000/0000 */
		df->id = 0x0000;
		sc_append_file_id(&df->path, df->id);
		r = card->ops->create_file(card, df);
		if (r != SC_SUCCESS) break;

		/* Create USER PIN and SO PIN*/
		r = create_pins(card);
		if (r != SC_SUCCESS) break;

		/* VERIFY USER PIN */
		r = sc_verify(card, SC_AC_CHV, do_pins[0].id, 
				do_pins[0].pass, sizeof(do_pins[0].pass), NULL);
		if (r != SC_SUCCESS) break;

		/* Create     3F00/0000/0000/0001 */
		df->id = 0x0001;
		sc_append_file_id(&df->path, df->id);
		r = card->ops->create_file(card, df);
		if (r != SC_SUCCESS) break;

		sc_format_path("3F0000000000", &df->path);
		r = card->ops->select_file(card, &df->path, NULL);
		if (r != SC_SUCCESS) break;

		/* Create     3F00/0000/0000/0002 */
		df->id = 0x0002;
		sc_append_file_id(&df->path, df->id);
		r = card->ops->create_file(card, df);
		if (r != SC_SUCCESS) break;

		sc_format_path("3F000000", &df->path);
		r = card->ops->select_file(card, &df->path, NULL);
		if (r != SC_SUCCESS) break;

		/* Create     3F00/0000/0001 */
		df->id = 0x0001;
		sc_append_file_id(&df->path, df->id);
		r = card->ops->create_file(card, df);
		if (r != SC_SUCCESS) break;

		/* RESET ACCESS RIGHTS */
		r = sc_logout(card);
	} while(0);
	sc_file_free(df);
	return r;
}

/* 
 * Card-specific initialization of PKCS15 profile information
 */
static int rutoken_init(sc_profile_t *profile, sc_card_t *card)
{
	struct file_info *ef_list;
	sc_file_t *df, *ef;
	size_t i;
	int r, ret = SC_SUCCESS;

	SC_FUNC_CALLED(card->ctx, 1);

	df = sc_file_new();
	if (!df)
	{
		sc_debug(card->ctx, "Failed to create file\n");
		return SC_ERROR_OUT_OF_MEMORY;
	}
	df->type = SC_FILE_TYPE_DF;
	r = sc_file_set_sec_attr(df, df_sec_attr, SEC_ATTR_SIZE);
	if (r != SC_SUCCESS)
		sc_debug(card->ctx, "Failed to set secure attr: %s\n", sc_strerror(r));

	for (ef_list = profile->ef_list; ef_list; ef_list = ef_list->next)
	{
		if (!ef_list->file  ||  ef_list->file->path.len <= 2)
			continue;
		df->path = ef_list->file->path;
		df->path.len -= 2;
		ret = card->ops->select_file(card, &df->path, NULL);
		if (ret != SC_SUCCESS)
		{
			sc_debug(card->ctx,"Failed select file: %s\n", sc_strerror(ret));
			break;
		}

		sc_file_dup(&ef, ef_list->file);
		if (!ef)
		{
			sc_debug(card->ctx, "Failed to dup file\n");
			ret = SC_ERROR_OUT_OF_MEMORY;
			break;
		}
		r = sc_file_set_sec_attr(ef, 
				ef->type == SC_FILE_TYPE_DF ? 
				df_sec_attr : ef_sec_attr, SEC_ATTR_SIZE);
	        if (r != SC_SUCCESS)
			sc_debug(card->ctx, "Failed to set secure attr: %s\n",
					sc_strerror(r));

		ret = card->ops->create_file(card, ef);
		sc_file_free(ef);
		if (ret != SC_SUCCESS)
		{
			sc_error(card->ctx, "Failed to create file "
					"in compliance with profile: %s\n",
					sc_strerror(ret));
			break;
		}

		for (i = 0; i < sizeof(arr_def_df)/sizeof(arr_def_df[0]); ++i)
			if (arr_def_df[i].dir != AODFid
				&&  strcasecmp(ef_list->ident, arr_def_df[i].name) == 0
			)
			{
				df->id = arr_def_df[i].dir;
				r = sc_append_file_id(&df->path, df->id);
				if (r == SC_SUCCESS)
					r = card->ops->create_file(card, df);
				if (r != SC_SUCCESS)
					sc_error(card->ctx, "Failed to create df, %s\n",
							sc_strerror(r));
				break;
			}
	}
	sc_file_free(df);
	return ret;
}

/*
 * Erase everything that's on the card
 * And create PKCS15 profile
 */
static int
rutoken_erase(struct sc_profile *profile, sc_card_t *card)
{
	int ret, ret_end;

        if (!profile || !card || !card->ctx || !card->ops
			|| !card->ops->select_file || !card->ops->create_file
	)
		return SC_ERROR_INVALID_ARGUMENTS;

	SC_FUNC_CALLED(card->ctx, 1);

	/* ret = sc_card_ctl(card, SC_CARDCTL_ERASE_CARD, NULL); */
	ret = sc_card_ctl(card, SC_CARDCTL_RUTOKEN_FORMAT_INIT, NULL);
	if (ret != SC_SUCCESS)
		sc_error(card->ctx, "Failed to erase: %s\n", sc_strerror(ret));
	else
	{
		ret = create_typical_fs(card);
		if (ret != SC_SUCCESS)
			sc_error(card->ctx, "Failed to create typical fs: %s\n",
					sc_strerror(ret));
		ret_end = sc_card_ctl(card, SC_CARDCTL_RUTOKEN_FORMAT_END, NULL);
		if (ret_end != SC_SUCCESS)
			ret = ret_end;
		if (ret == SC_SUCCESS)
		{
			/* VERIFY __default__ USER PIN */
			/* assert(sizeof(do_pins)/sizeof(do_pins[0]) >= 1); */
			/* assert(do_pins[0].id == SC_RUTOKEN_DEF_ID_GCHV_USER); */
			ret = sc_verify(card, SC_AC_CHV, do_pins[0].id,
					do_pins[0].pass, sizeof(do_pins[0].pass), NULL);
			if (ret != SC_SUCCESS)
				sc_debug(card->ctx, "VERIFY default USER PIN: %s\n",
						sc_strerror(ret));
			else
			{
				ret = rutoken_init(profile, card);

				/* RESET ACCESS RIGHTS */
				if (sc_logout(card) != SC_SUCCESS)
					sc_debug(card->ctx,
							"Failed RESET ACCESS RIGHTS\n");
			}
		}
		if (ret != SC_SUCCESS)
			sc_error(card->ctx, "Failed to init PKCS15: %s\n",
					sc_strerror(ret));
	}
	return ret;
}

static struct sc_pkcs15init_operations sc_pkcs15init_rutoken_operations = {
	rutoken_erase,                  /* erase_card */
	NULL,                           /* init_card */
	rutoken_create_dir,             /* create_dir */
	NULL,                           /* create_domain */
	NULL,                           /* select_pin_reference */
	NULL,                           /* create_pin */
	rutoken_select_key_reference,   /* select_key_reference */
	rutoken_create_key,             /* create_key */
	rutoken_store_key,              /* store_key */
	NULL,                           /* generate_key */
	rutoken_encode_private_key,     /* encode_private_key */
	NULL,                           /* encode_public_key */
	NULL,                           /* finalize_card */
	/* Old-style API */
	NULL,                           /* init_app */
	NULL,                           /* new_pin */
	NULL,                           /* new_key */
	rutoken_new_file,               /* new_file */
	NULL,                           /* old_generate_key */
	NULL                            /* delete_object */
};

struct sc_pkcs15init_operations* sc_pkcs15init_get_rutoken_ops(void)
{
	return &sc_pkcs15init_rutoken_operations;
}
