#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h> // inet_addr
#include "devx_prm.h"
#include "mlx5_ifc.h"
#include "cgmk_utils.h"

#define MLX5_GENERAL_OBJ_TYPE_MKEY 0xff01
#define MLX5_MKEY_ACCESS_MODE_CROSSING_VHCA 0x6

#define DEVX_CAP_GEN(hca_cur, cap) \
	DEVX_GET(cmd_hca_cap, hca_cur, cap)

#define DEVX_CAP_GEN_2(hca_cur, cap) \
	DEVX_GET(cmd_hca_cap_2, hca_cur, cap)

enum mlx5_cap_type {
	MLX5_CAP_GENERAL = 0,
	MLX5_CAP_GENERAL_2 = 0x20,
	MLX5_CAP_NUM,
};

int
query_hca_caps(struct ibv_context *context, enum mlx5_cap_type cap_type, uint32_t *hca_cur)
{
	int ret;
	uint32_t in[DEVX_ST_SZ_DW(query_hca_cap_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(query_hca_cap_out)] = {0};
	uint16_t opmod = (cap_type << 1) | (1 & 0x01); // HCA_CAP_OPMOD_GET_CUR == 1
	void *hca_caps;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod, opmod);
	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in), out, sizeof(out));
	if (ret) {
		fprintf(stderr, "QUERY_HCA_CAP: type(%x) opmode(cur) Failed(%d)\n",
				MLX5_CAP_GENERAL, ret);
		return -1;
	}
	hca_caps = DEVX_ADDR_OF(query_hca_cap_out, out, capability);
	memcpy(hca_cur, hca_caps, DEVX_UN_SZ_BYTES(hca_cap_union));
	return 0;
}

/*
 * Alias to remote cross gvmi mkey
 */
struct cgmk_mr_crossing {
	struct ibv_pd  		*pd;
	struct mlx5dv_devx_obj  *alias_obj;
	uint32_t 		lkey;
	void                    *addr;
	size_t                  length;

};

/*
 * DEVX MKEY container used by Windows/Linux Host for umem based mkey allocation
 */
struct cgmk_mkey {
	struct ibv_context 		*context;
	struct mlx5dv_devx_umem 	*umem;
	struct mlx5dv_devx_obj 		*mkey_obj;
	int 				 lkey;
	void 				*addr;
	size_t 				 length;
};

/*
 * Creates UMEM + DEVX MKEY (virt addr) in system memory
 */
struct cgmk_mkey*
create_cgmk_mkey(struct ibv_pd *pd, void *buf, size_t buf_sz)
{
	int ret;
	struct cgmk_mkey *out_mkey = (struct cgmk_mkey*)calloc(1, sizeof(struct cgmk_mkey));
	out_mkey->context = pd->context;
	out_mkey->addr = buf;
	out_mkey->length = buf_sz;
	// Reg umem buf
	out_mkey->umem = mlx5dv_devx_umem_reg(pd->context, buf, buf_sz, 7);
	if (!out_mkey->umem) {
		fprintf(stderr, "Failed to register umem buffer.\n");
		return NULL;
	}

	// Cast pd to dv
	struct mlx5dv_obj *pd_obj = &(struct mlx5dv_obj){
		.pd.in = pd,
		.pd.out = &(struct mlx5dv_pd){0},
	};
	ret = mlx5dv_init_obj(pd_obj, MLX5DV_OBJ_PD);
	if (ret) {
		fprintf(stderr, "Not able to expose pdn from pd.\n");
		return NULL;
	}   

	uint32_t in[DEVX_ST_SZ_DW(create_mkey_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(create_mkey_out)] = {0};
	void *mkc;

	DEVX_SET(create_mkey_in, in, opcode, MLX5_CMD_OP_CREATE_MKEY);
	DEVX_SET(create_mkey_in, in, translations_octword_actual_size, 1);
	DEVX_SET(create_mkey_in, in, pg_access, 1);
	DEVX_SET(create_mkey_in, in, mkey_umem_id, out_mkey->umem->umem_id);
	mkc = DEVX_ADDR_OF(create_mkey_in, in, memory_key_mkey_entry);
	DEVX_SET(mkc, mkc, a, 0x1); // atomic
	DEVX_SET(mkc, mkc, lw, 0x1);
	DEVX_SET(mkc, mkc, lr, 0x1);
	DEVX_SET(mkc, mkc, rw, 0x1);
	DEVX_SET(mkc, mkc, rr, 0x1);
	DEVX_SET(mkc, mkc, mkey_7_0, 0x42);
	DEVX_SET(mkc, mkc, qpn, 0xffffff);
	DEVX_SET(mkc, mkc, pd, pd_obj->pd.out->pdn);
	DEVX_SET(mkc, mkc, access_mode_1_0, MLX5_MKC_ACCESS_MODE_MTT);
	DEVX_SET(mkc, mkc, translations_octword_size_crossing_target_mkey, 1);
	DEVX_SET(mkc, mkc, log_entity_size, 12);
	DEVX_SET64(mkc, mkc, start_addr, (intptr_t)buf); // virtual address
	DEVX_SET64(mkc, mkc, len, buf_sz); // region address

	out_mkey->mkey_obj = mlx5dv_devx_obj_create(pd->context, in, sizeof(in), out, sizeof(out));
	if (!out_mkey->mkey_obj) {
		fprintf(stderr, "Failed to create devx mkey.\n");
		return NULL;
	}
	out_mkey->lkey = DEVX_GET(create_mkey_out, out, mkey_index) << 8 | 0x42; // extend mkey index to 32 bits
	
	return out_mkey;
}

int
dereg_cgmk_mkey(struct cgmk_mkey *obj) {
	mlx5dv_devx_obj_destroy(obj->mkey_obj);
	mlx5dv_devx_umem_dereg(obj->umem);
	free(obj);
	return 0;
}

/*
 * Allow Cross GVMI on target mr/mkey.
 * Params:
 *      mr              - target mr
 *      token           - access key
 *      token_sz        - size of token, up to 256 bits
 *      out_mr_desc     - user allocated buffer for returned serealized data
 * return: size of written data to the "out_mr_desc" param
 */
size_t
cgmk_mr_export(
#ifndef IBVMR
		struct cgmk_mkey *mr,
#else
		struct ibv_mr *mr,
#endif
		char *token, size_t token_sz, 
		char *out_mr_desc, size_t out_mr_desc_sz)
{
	int ret;
	struct desc_data out_data;
	out_data.mkey = mr->lkey;
	out_data.buf = mr->addr;
	out_data.buf_size = mr->length;
	out_data.access_key_sz = token_sz;
	memcpy(out_data.access_key, token, token_sz);

	// Check that HCA.cross_vhca_object_to_object is supported.
	uint32_t hca_caps_2[DEVX_UN_SZ_DW(hca_cap_union)] = {0};
	ret = query_hca_caps(mr->context, MLX5_CAP_GENERAL_2, hca_caps_2);
	if (ret) {
		fprintf(stderr, "Failed to query HCA.cross_vhca_object_to_object.\n");
		return 0;
	}
	if (!(DEVX_CAP_GEN_2(hca_caps_2, cross_vhca_object_to_object_supported) & 0x100)) {
		fprintf(stderr, "Cross GVMI MKEY not enabled. Cross_vhca_object_to_object_supported: %x\n",
				DEVX_CAP_GEN_2(hca_caps_2, cross_vhca_object_to_object_supported));
		return 0;
	}

	// Allow crossing mkey
	uint32_t in[DEVX_ST_SZ_DW(allow_other_vhca_access_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(allow_other_vhca_access_out)] = {0};
	void *access_key;

	if (token_sz > 255) {
		fprintf(stderr, "Token size exceeds the limit.\n");
		return 0;
	}

	DEVX_SET(allow_other_vhca_access_in, in, opcode, MLX5_CMD_OP_ALLOW_OTHER_VHCA_ACCESS);
	DEVX_SET(allow_other_vhca_access_in, in, object_type_to_be_accessed, MLX5_GENERAL_OBJ_TYPE_MKEY);
	DEVX_SET(allow_other_vhca_access_in, in, object_id_to_be_accessed, out_data.mkey >> 8); // needs only 24 bits(mk idx), discard last 8 bits
	access_key = DEVX_ADDR_OF(allow_other_vhca_access_in, in, access_key);
	memcpy(access_key, token, sizeof(char) * token_sz);

	ret = mlx5dv_devx_general_cmd(mr->context, in, sizeof(in), out, sizeof(out));
	if (ret) {
		fprintf(stderr, "Failed to allow other vhca access.\n");
		return 0;
	}

	// Get vhca id
	uint32_t hca_caps[DEVX_UN_SZ_DW(hca_cap_union)] = {0};
	ret = query_hca_caps(mr->context, MLX5_CAP_GENERAL, hca_caps);
	if (ret) {
		fprintf(stderr, "Failed to query vhca id.\n");
		return 0;
	}
	out_data.vhca_id = DEVX_CAP_GEN(hca_caps, vhca_id);	

	// Serialize
	return serialize_desc_data(&out_data, out_mr_desc, out_mr_desc_sz);
}

/*
 * Remote crossing mkey builder, which uses DEVX alias object of mkey type
 * NOTE: you must call @dereg_cgmk_mr_crossing() to properly dereg crossing mkey object.
 * Params:
 * 	pd - local pd
 *	crossing_mr_desc - remote serialized shared mr desc
 *	crossing_mr_desc_sz - size of serialized region
 * return: pointer to the alias object
 */
struct cgmk_mr_crossing*
cgmk_mr_crossing_reg(struct ibv_pd *pd, char *crossing_mr_desc, size_t crossing_mr_desc_sz)
{
	// Parse desc str
	int ret;
	struct desc_data mr_cr_data = {0};
	ret = deserialize_desc_data(crossing_mr_desc, crossing_mr_desc_sz, &mr_cr_data);

	// Cast pd to dv
	struct mlx5dv_obj *pd_obj = &(struct mlx5dv_obj){
		.pd.in = pd,
		.pd.out = &(struct mlx5dv_pd){0},
	};
	ret = mlx5dv_init_obj(pd_obj, MLX5DV_OBJ_PD);
	if (ret) {
		fprintf(stderr, "Not able to expose pdn from pd.\n");
		return NULL;
	}

	// Create alias
	struct mlx5dv_devx_obj *alias;
	uint32_t in[DEVX_ST_SZ_DW(create_alias_obj_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(create_alias_obj_out)] = {0};

	void *hdr = DEVX_ADDR_OF(create_alias_obj_in, in, hdr);
	void *alias_ctx = DEVX_ADDR_OF(create_alias_obj_in, in, alias_ctx);

	DEVX_SET(general_obj_in_cmd_hdr, hdr, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	DEVX_SET(general_obj_in_cmd_hdr, hdr, obj_type, MLX5_GENERAL_OBJ_TYPE_MKEY);
	DEVX_SET(general_obj_in_cmd_hdr, hdr, alias_object, 1);
	DEVX_SET(alias_context, alias_ctx, vhca_id_to_be_accessed, mr_cr_data.vhca_id);
	DEVX_SET(alias_context, alias_ctx, object_id_to_be_accessed, mr_cr_data.mkey >> 8); // needs only 24 bits(mk idx), discard last 8 bits
	void *access_key = DEVX_ADDR_OF(alias_context, alias_ctx, access_key);
	memcpy(access_key, mr_cr_data.access_key, mr_cr_data.access_key_sz);
	DEVX_SET(alias_context, alias_ctx, metadata, pd_obj->pd.out->pdn);

	alias = mlx5dv_devx_obj_create(pd->context, in, sizeof(in), out, sizeof(out));
	if (!alias) {
		fprintf(stderr, "Failed to create MKEY Alias Object.\n");
		return NULL;
	}
	ret = DEVX_GET(create_alias_obj_out, out, alias_ctx.status);
	if (ret) {
		fprintf(stderr, "Created mr alias object in bad state.\n");
		return NULL;
	}

	// Create crossing mr
	struct cgmk_mr_crossing *mr_crossing;
	mr_crossing = (struct cgmk_mr_crossing*)calloc(1, sizeof(*mr_crossing));
	mr_crossing->pd = pd;
	mr_crossing->alias_obj = alias;
	mr_crossing->lkey = DEVX_GET(create_alias_obj_out, out, hdr.obj_id) << 8; // lkey size is 32 bits (alias id == mkey idx == 24 bits)
	mr_crossing->addr = mr_cr_data.buf;
	mr_crossing->length = mr_cr_data.buf_size;
	
	return mr_crossing;
}

int
dereg_cgmk_mr_crossing(struct cgmk_mr_crossing *mr)
{
	mlx5dv_devx_obj_destroy(mr->alias_obj);
	free(mr);
	return 0;
}
