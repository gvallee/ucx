/*
* Copyright (C) Mellanox Technologies Ltd, 2001-2018. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifndef __DEVX_PRM_H__
#define __DEVX_PRM_H__

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <endian.h>

#define u8 uint8_t
#define BIT(n) (1<<(n))
#define __packed
#include <linux/types.h>
#include "ib_mlx5_ifc.h"

struct mlx5_ifc_pas_umem_bits {
	u8	   reserved_at_0[0x20];
	u8	   pas_umem_id[0x20];
	u8	   pas_umem_off[0x40];
	u8	   reserved_at_80[0x20];
	u8	   dbr_umem_id[0x20];
	u8	   dbr_umem_off[0x40];
};

#if !HAVE_DECL___DEVX_NULLP

#ifndef __devx_nullp
#define __devx_nullp(typ) ((struct mlx5_ifc_##typ##_bits *)0)
#endif

#ifndef __devx_bit_sz
#define __devx_bit_sz(typ, fld) sizeof(__devx_nullp(typ)->fld)
#endif

#ifndef __devx_bit_off
#define __devx_bit_off(typ, fld) (offsetof(struct mlx5_ifc_##typ##_bits, fld))
#endif

#ifndef __devx_dw_off
#define __devx_dw_off(typ, fld) (__devx_bit_off(typ, fld) / 32)
#endif

#ifndef __devx_64_off
#define __devx_64_off(typ, fld) (__devx_bit_off(typ, fld) / 64)
#endif

#ifndef __devx_dw_bit_off
#define __devx_dw_bit_off(typ, fld) (32 - __devx_bit_sz(typ, fld) - (__devx_bit_off(typ, fld) & 0x1f))
#endif

#ifndef __devx_mask
#define __devx_mask(typ, fld) ((uint32_t)((1ull << __devx_bit_sz(typ, fld)) - 1))
#endif

#ifndef __devx_dw_mask
#define __devx_dw_mask(typ, fld) (__devx_mask(typ, fld) << __devx_dw_bit_off(typ, fld))
#endif

#ifndef __devx_st_sz_bits
#define __devx_st_sz_bits(typ) sizeof(struct mlx5_ifc_##typ##_bits)
#endif

#ifndef DEVX_FLD_SZ_BYTES
#define DEVX_FLD_SZ_BYTES(typ, fld) (__devx_bit_sz(typ, fld) / 8)
#endif

#ifndef DEVX_ST_SZ_BYTES
#define DEVX_ST_SZ_BYTES(typ) (sizeof(struct mlx5_ifc_##typ##_bits) / 8)
#endif

#ifndef DEVX_ST_SZ_DW
#define DEVX_ST_SZ_DW(typ) (sizeof(struct mlx5_ifc_##typ##_bits) / 32)
#endif

#ifndef DEVX_ST_SZ_QW
#define DEVX_ST_SZ_QW(typ) (sizeof(struct mlx5_ifc_##typ##_bits) / 64)
#endif

#ifndef DEVX_UN_SZ_BYTES
#define DEVX_UN_SZ_BYTES(typ) (sizeof(union mlx5_ifc_##typ##_bits) / 8)
#endif

#ifndef DEVX_UN_SZ_DW
#define DEVX_UN_SZ_DW(typ) (sizeof(union mlx5_ifc_##typ##_bits) / 32)
#endif

#ifndef DEVX_BYTE_OFF
#define DEVX_BYTE_OFF(typ, fld) (__devx_bit_off(typ, fld) / 8)
#endif 

#ifndef DEVX_ADDR_OF
#define DEVX_ADDR_OF(typ, p, fld) ((unsigned char *)(p) + DEVX_BYTE_OFF(typ, fld))
#endif

#ifndef BUILD_BUG_ON
#define BUILD_BUG_ON(a) /*TODO*/
#endif


/* insert a value to a struct */
#ifndef DEVX_SET
#define DEVX_SET(typ, p, fld, v) do { \
	uint32_t _v = v; \
	BUILD_BUG_ON(__devx_st_sz_bits(typ) % 32);   \
	*((__be32 *)(p) + __devx_dw_off(typ, fld)) = \
	htobe32((be32toh(*((__be32 *)(p) + __devx_dw_off(typ, fld))) & \
		     (~__devx_dw_mask(typ, fld))) | (((_v) & __devx_mask(typ, fld)) \
		     << __devx_dw_bit_off(typ, fld))); \
} while (0)
#endif

#ifndef DEVX_GET
#define DEVX_GET(typ, p, fld) ((be32toh(*((__be32 *)(p) +\
	__devx_dw_off(typ, fld))) >> __devx_dw_bit_off(typ, fld)) & \
	__devx_mask(typ, fld))
#endif
#endif

#ifndef __DEVX_SET64
#define __DEVX_SET64(typ, p, fld, v) do { \
	BUILD_BUG_ON(__devx_bit_sz(typ, fld) != 64); \
	*((__be64 *)(p) + __devx_64_off(typ, fld)) = htobe64(v); \
} while (0)
#endif

#ifndef DEVX_SET64
#define DEVX_SET64(typ, p, fld, v) do { \
	BUILD_BUG_ON(__devx_bit_off(typ, fld) % 64); \
	__DEVX_SET64(typ, p, fld, v); \
} while (0)
#endif

#ifndef DEVX_GET64
#define DEVX_GET64(typ, p, fld) \
	be64toh(*((__be64 *)(p) + __devx_64_off(typ, fld)))
#endif

#define DEVX_SET_TO_ONES(typ, p, fld) do { \
	BUILD_BUG_ON(__devx_st_sz_bits(typ) % 32);	       \
	*((__be32 *)(p) + __devx_dw_off(typ, fld)) = \
	htobe32((be32toh(*((__be32 *)(p) + __devx_dw_off(typ, fld))) & \
		     (~__devx_dw_mask(typ, fld))) | ((__devx_mask(typ, fld)) \
		     << __devx_dw_bit_off(typ, fld))); \
} while (0)

#endif
