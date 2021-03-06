#include <sys/time.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>

#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/ripemd.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>

#include "encounter_priv.h"

#include "openssl_drv.h"

#include "utils.h"

typedef struct seed_ss {
	uint32_t	r[32];	/* 1024-bit */
} seed_t;

#define BN_is_not_one(a)      (!(BN_is_one(a)))
#define BN_are_equal(a,b)     (BN_cmp(a,b) == 0)
#define BN_are_not_equal(a,b) (!(BN_are_equal(a,b)))
#define BN_is_neg(a)          (a->neg == 1)


/* Some static prototypes */
static encounter_err_t rng_init (void);

static encounter_err_t encounter_crypto_openssl_new_keyctx(\
			const encounter_key_t, ec_keyctx_t **);

static encounter_err_t encounter_crypto_openssl_invMod2toW(\
	encounter_t *,	BIGNUM *, const BIGNUM *, BN_CTX *);

static encounter_err_t encounter_crypto_openssl_hConstant (\
	encounter_t *, BIGNUM *, const BIGNUM *, const BIGNUM *, \
			const BIGNUM *, const BIGNUM *, BN_CTX *);

static encounter_err_t encounter_crypto_openssl_fastL(\
	encounter_t *, BIGNUM *, const BIGNUM *, const BIGNUM *, \
      					const BIGNUM *, BN_CTX *);

static encounter_err_t encounter_crypto_openssl_paillierEncrypt(\
	encounter_t *, BIGNUM *, const BIGNUM *, const ec_keyctx_t *);

static encounter_err_t IsInZnstar(encounter_t *, const BIGNUM *, \
				const BIGNUM *, BN_CTX *, bool *);

static encounter_err_t IsInZnSquaredstar(encounter_t *, \
	     const BIGNUM *a, const BIGNUM *n, BN_CTX *bnctx, bool *);

static encounter_err_t encounter_crypto_openssl_paillierUpdate(\
	encounter_t *, BIGNUM *, const ec_keyctx_t *, BN_CTX *, \
	const unsigned int, bool );

static encounter_err_t encounter_crypto_openssl_paillierAddSub(\
    encounter_t *, BIGNUM *, BIGNUM *, const ec_keyctx_t *, \
                                        BN_CTX *, const bool);

static encounter_err_t encounter_crypto_openssl_paillierMul(\
	encounter_t *, BIGNUM *, const ec_keyctx_t *, BN_CTX *, \
	const unsigned int, bool);

static encounter_err_t encounter_crypto_openssl_fastCRT(\
	encounter_t *, BIGNUM *g, const BIGNUM *g1, const BIGNUM *p, \
	const BIGNUM *g2, const BIGNUM *q, const BIGNUM *qInv, \
							BN_CTX *bnctx);

static encounter_err_t encounter_crypto_openssl_new_paillierGenerator(\
			encounter_t *,	BIGNUM *, const ec_keyctx_t *);

static encounter_err_t encounter_crypto_openssl_qInv(encounter_t *ctx, \
		BIGNUM *,  const BIGNUM *, const BIGNUM *, BN_CTX *);




static encounter_err_t rng_init(void)
{
        seed_t  *seed_p;
        int c;

        seed_p = malloc(sizeof *seed_p);
        if(!seed_p) return (ENCOUNTER_ERR_MEM);

#ifdef HAVE_ARC4RANDOM
        for (int i = 0; i < (sizeof *seed_p/sizeof seed_p->r[0]); ++i)
                seed_p->r[i] = arc4random_uniform(UINT32_MAX);
#else   /* Revert on /dev/urandom */
        do {
                FILE *f = fopen("/dev/urandom", "r");
                if (!f) return (ENCOUNTER_ERR_OS);
                fread(seed_p, sizeof *seed_p, 1, f);
                fclose(f);

        } while(0);

#endif
        RAND_seed(seed_p, sizeof *seed_p);

        c = RAND_status();
        if (seed_p) free(seed_p);
        if (c == 0) return ENCOUNTER_ERR_CRYPTO;

        return (ENCOUNTER_OK);
}

encounter_err_t encounter_crypto_openssl_init(encounter_t *ctx)
{
	EVP_add_cipher(EVP_aes_256_cbc());
	EVP_add_digest(EVP_ripemd160());

	if (rng_init() != ENCOUNTER_OK) {
		ctx->rc = ENCOUNTER_ERR_CRYPTO;
		return ctx->rc;
	}

#ifdef __ENCOUNTER_DEBUG_
	CRYPTO_malloc_debug_init();
	CRYPTO_set_mem_debug_options(V_CRYPTO_MDEBUG_ALL);
	CRYPTO_mem_ctrl(CRYPTO_MEM_CHECK_ON);
	ERR_load_crypto_strings();
#endif /* ! __ENCOUNTER_DEBUG_ */

	/* Set ctx->m to zero. This precomputed quantity will be used
	 * to initialize each crypto counter instance */
	ctx->m = BN_new();
        if (!BN_zero(ctx->m)) OPENSSL_ERROR(end);

	ctx->rc = ENCOUNTER_OK;

end:
	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_term(encounter_t *ctx)
{
	BN_free(ctx->m); /* Free the crypto counter initializer */
	EVP_cleanup();	 /* Cleanup the OpenSSL environment */

#ifdef __ENCOUNTER_DEBUG_
	printf("--------Memory Leaks displayed below--------\n");
	CRYPTO_cleanup_all_ex_data();
	ERR_free_strings();
	ERR_remove_state(0);
	CRYPTO_mem_leaks_fp(stdout);
	printf("--------Memory Leaks displayed above--------\n");
#endif /* ! __ENCOUNTER_DEBUG_ */

	ctx->rc = ENCOUNTER_OK;
	return ctx->rc;
}


static encounter_err_t encounter_crypto_openssl_new_keyctx( \
			const encounter_key_t type, ec_keyctx_t **keyctx_pp) 
{
	encounter_err_t rc;
	ec_keyctx_t *key_p = NULL;

	if (keyctx_pp)  {
		key_p = calloc(1, sizeof *key_p);
		if (key_p) {
			key_p->type = type;
			switch (type) {
				case EC_KEYTYPE_PAILLIER_PUBLIC:
					key_p->k.paillier_pubK.n = BN_new();
					key_p->k.paillier_pubK.g = BN_new();
					key_p->k.paillier_pubK.nsquared = \
								BN_new();
					if (    key_p->k.paillier_pubK.n \
					     && key_p->k.paillier_pubK.g \
					     && key_p->k.paillier_pubK.nsquared)
						rc = ENCOUNTER_OK;
					else
						rc = ENCOUNTER_ERR_MEM; 
					break;

				case EC_KEYTYPE_PAILLIER_PRIVATE:
					key_p->k.paillier_privK.p = BN_new();
					key_p->k.paillier_privK.q = BN_new();
					key_p->k.paillier_privK.psquared = BN_new();
					key_p->k.paillier_privK.qsquared = BN_new();
					key_p->k.paillier_privK.pinvmod2tow = BN_new();
					key_p->k.paillier_privK.qinvmod2tow = BN_new();
					key_p->k.paillier_privK.hsubp = BN_new();
					key_p->k.paillier_privK.hsubq = BN_new();
					key_p->k.paillier_privK.qInv = BN_new();

					if (   key_p->k.paillier_privK.p \
					    && key_p->k.paillier_privK.q \
					    && key_p->k.paillier_privK.psquared\
					    && key_p->k.paillier_privK.qsquared\
					    && key_p->k.paillier_privK.pinvmod2tow \
					    && key_p->k.paillier_privK.qinvmod2tow \
					    && key_p->k.paillier_privK.hsubp \
					    && key_p->k.paillier_privK.hsubq \
					    && key_p->k.paillier_privK.qInv )
						rc = ENCOUNTER_OK;
					else
						rc = ENCOUNTER_ERR_MEM;
					break;

				default:
					free(key_p);
					key_p = NULL;
					/* ctx->rc = ENCOUNTER_ERR_MEM; */
					return ENCOUNTER_ERR_PARAM;

			}
			/* Copy out the pointer to the key context */
			*keyctx_pp = key_p;

		} else
			rc = ENCOUNTER_ERR_MEM;

	}	else rc = ENCOUNTER_ERR_PARAM;

	/* We are done */
	return rc;
}

encounter_err_t encounter_crypto_openssl_free_keyctx(encounter_t *ctx, ec_keyctx_t *keyctx) {
        if (!ctx) return ENCOUNTER_ERR_PARAM;

	if (keyctx) {
		switch (keyctx->type) {
			case EC_KEYTYPE_PAILLIER_PUBLIC:
				BN_free(keyctx->k.paillier_pubK.n);
				BN_free(keyctx->k.paillier_pubK.g);
				BN_free(keyctx->k.paillier_pubK.nsquared);
				break;

			case EC_KEYTYPE_PAILLIER_PRIVATE:
				BN_free(keyctx->k.paillier_privK.p);
				BN_free(keyctx->k.paillier_privK.q);
				BN_free(keyctx->k.paillier_privK.psquared);
				BN_free(keyctx->k.paillier_privK.qsquared);
				BN_free(keyctx->k.paillier_privK.pinvmod2tow);
				BN_free(keyctx->k.paillier_privK.qinvmod2tow);
				BN_free(keyctx->k.paillier_privK.hsubp);
				BN_free(keyctx->k.paillier_privK.hsubq);
				BN_free(keyctx->k.paillier_privK.qInv);
				break;
			default:
				ctx->rc = ENCOUNTER_ERR_PARAM;
				return ctx->rc;
		}

		free(keyctx);
		ctx->rc = ENCOUNTER_OK;
	} else ctx->rc = ENCOUNTER_ERR_PARAM;

	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_keygen(encounter_t *ctx, \
	encounter_key_t type, unsigned int keysize, ec_keyctx_t **pubK, ec_keyctx_t **privK) 
{
	encounter_err_t rc;

        if (!ctx)       return ENCOUNTER_ERR_PARAM;

	__ENCOUNTER_SANITYCHECK_KEYTYPE(type, ENCOUNTER_ERR_PARAM);
	__ENCOUNTER_SANITYCHECK_KEYSIZE(keysize, ENCOUNTER_ERR_PARAM);

	BN_CTX *bnctx = BN_CTX_new();


	rc = encounter_crypto_openssl_new_keyctx(\
				EC_KEYTYPE_PAILLIER_PUBLIC, pubK);
	if (rc != ENCOUNTER_OK) goto err;	

	rc = encounter_crypto_openssl_new_keyctx(\
				EC_KEYTYPE_PAILLIER_PRIVATE, privK);
	if (rc != ENCOUNTER_OK) goto err;	

	/* Generate p and q primes */
	if (!BN_generate_prime((*privK)->k.paillier_privK.p, keysize,0,\
					NULL, NULL, NULL, NULL) )
		OPENSSL_ERROR(err);

	if (!BN_generate_prime((*privK)->k.paillier_privK.q, keysize,0,\
					NULL, NULL, NULL, NULL) )
		OPENSSL_ERROR(err);

	/* p^2 */
	if (!BN_sqr((*privK)->k.paillier_privK.psquared, \
	       (*privK)->k.paillier_privK.p, bnctx) )
		OPENSSL_ERROR(err);

	/* q^2 */
	if (!BN_sqr((*privK)->k.paillier_privK.qsquared, \
	       (*privK)->k.paillier_privK.q, bnctx) )
		OPENSSL_ERROR(err);

	/* n = pq */
	if (!BN_mul((*pubK)->k.paillier_pubK.n,    \
		(*privK)->k.paillier_privK.p, \
		(*privK)->k.paillier_privK.q, bnctx) )
		OPENSSL_ERROR(err);

	/* n^2 */
	if (!BN_sqr(	(*pubK)->k.paillier_pubK.nsquared, \
	        (*pubK)->k.paillier_pubK.n, bnctx) )
		OPENSSL_ERROR(err);

	/* Generate the Paillier generator */
	if (encounter_crypto_openssl_new_paillierGenerator( ctx, \
		(*pubK)->k.paillier_pubK.g, *privK) != ENCOUNTER_OK)
		OPENSSL_ERROR(err);	/* blame OpenSSL... */

	/* _p = p^-1 mod 2^w */
	if (encounter_crypto_openssl_invMod2toW(ctx, \
	  	(*privK)->k.paillier_privK.pinvmod2tow, \
		(*privK)->k.paillier_privK.p, bnctx) != ENCOUNTER_OK)
		OPENSSL_ERROR(err);

	/* _q = q^-1 mod 2^w */
	if (encounter_crypto_openssl_invMod2toW(ctx, \
		(*privK)->k.paillier_privK.qinvmod2tow, \
		(*privK)->k.paillier_privK.q, bnctx) != ENCOUNTER_OK )
		OPENSSL_ERROR(err);

	/* Compute H constants */
	/* h_p */
	if (encounter_crypto_openssl_hConstant(ctx, \
		(*privK)->k.paillier_privK.hsubp, \
		(*pubK)->k.paillier_pubK.g, 	\
		(*privK)->k.paillier_privK.p, 	\
		(*privK)->k.paillier_privK.psquared, \
		(*privK)->k.paillier_privK.pinvmod2tow, bnctx) \
	  != ENCOUNTER_OK)
		OPENSSL_ERROR(err);

	/* h_q */
	if (encounter_crypto_openssl_hConstant( ctx, \
		(*privK)->k.paillier_privK.hsubq, \
		(*pubK)->k.paillier_pubK.g, 	\
		(*privK)->k.paillier_privK.q, 	\
		(*privK)->k.paillier_privK.qsquared, \
		(*privK)->k.paillier_privK.qinvmod2tow, bnctx) \
	  != ENCOUNTER_OK)
		OPENSSL_ERROR(err);
 
	/* Q^-1 */
	if (encounter_crypto_openssl_qInv(ctx,\
		(*privK)->k.paillier_privK.qInv, \
		(*privK)->k.paillier_privK.q, \
		(*privK)->k.paillier_privK.p, bnctx) != ENCOUNTER_OK)
		OPENSSL_ERROR(err);


	if (bnctx) BN_CTX_free(bnctx);

	ctx->rc = ENCOUNTER_OK;
	return ctx->rc;

err:
	ctx->rc = rc;
	if (bnctx) BN_CTX_free(bnctx);
	if (*pubK) encounter_crypto_openssl_free_keyctx(ctx, *pubK);
	if (*privK) encounter_crypto_openssl_free_keyctx(ctx, *privK);

	return  ctx->rc;
}

static encounter_err_t encounter_crypto_openssl_new_paillierGenerator(\
		encounter_t *ctx, BIGNUM *g, const ec_keyctx_t *privK)
{
	if (!ctx)         return ENCOUNTER_ERR_PARAM;
	if (!g || !privK) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM,
                        "null param");
                goto end;
        }

	BIGNUM *tmp, *inv, *pmin1, *qmin1, *gsubp, *gsubq;
	bool in = false;
	BN_CTX *bnctx = BN_CTX_new();
	BN_CTX_start(bnctx);

	tmp = BN_CTX_get(bnctx); inv = BN_CTX_get(bnctx);
	pmin1 = BN_CTX_get(bnctx); qmin1 = BN_CTX_get(bnctx);
	gsubp = BN_CTX_get(bnctx);gsubq = BN_CTX_get(bnctx);

	if (!gsubq) OPENSSL_ERROR(end);

	/* p-1 and q-1 */
	if (!BN_sub(pmin1, privK->k.paillier_privK.p, BN_value_one()))
			OPENSSL_ERROR(end);
	if (!BN_sub(qmin1, privK->k.paillier_privK.q, BN_value_one()))
			OPENSSL_ERROR(end);

	/* g_p */
	for (;;) {
   		if (!BN_rand_range(gsubp, privK->k.paillier_privK.psquared))
			OPENSSL_ERROR(end);
   		if (IsInZnSquaredstar(ctx, gsubp, \
			privK->k.paillier_privK.psquared, bnctx, &in) \
				!= ENCOUNTER_OK )
			OPENSSL_ERROR(end);
		if (in)
      		{
      			if (!BN_mod_exp(tmp, gsubp, pmin1, \
				privK->k.paillier_privK.psquared, bnctx))
				OPENSSL_ERROR(end);
      			if (BN_are_not_equal(tmp, BN_value_one()))
         			break;
      		}
   	}
	/* g_q */
	for (;;) {
   		if (!BN_rand_range(gsubq, privK->k.paillier_privK.qsquared))
			OPENSSL_ERROR(end);
   		if (IsInZnSquaredstar(ctx, gsubq, \
			privK->k.paillier_privK.qsquared, bnctx, &in) \
				!= ENCOUNTER_OK )
			OPENSSL_ERROR(end);
		if (in)
      		{
      			if (!BN_mod_exp(tmp, gsubq, qmin1, \
				privK->k.paillier_privK.qsquared, bnctx))
				OPENSSL_ERROR(end);
      			if (BN_are_not_equal(tmp, BN_value_one()))
         			break;
      		}
   	}
	/* (q^2 mod p^2)^-1 */
	if (!BN_mod(tmp, privK->k.paillier_privK.qsquared, \
		privK->k.paillier_privK.psquared, bnctx))
		OPENSSL_ERROR(end);
	if (!BN_mod_inverse(inv, tmp, privK->k.paillier_privK.psquared, bnctx))
		OPENSSL_ERROR(end);
	if (encounter_crypto_openssl_fastCRT(ctx, g, gsubp,  \
		privK->k.paillier_privK.psquared, gsubq, \
		privK->k.paillier_privK.qsquared, inv, bnctx) \
			!= ENCOUNTER_OK)
		OPENSSL_ERROR(end);	

	ctx->rc = ENCOUNTER_OK;

end:

	if (tmp)   BN_clear(tmp);
	if (inv)   BN_clear(inv);
	if (pmin1) BN_clear(pmin1);
	if (qmin1) BN_clear(qmin1);
	if (gsubp) BN_clear(gsubp);
	if (gsubq) BN_clear(gsubq);

	if (bnctx) BN_CTX_end(bnctx);
	if (bnctx) BN_CTX_free(bnctx);

	return ctx->rc;
}

static encounter_err_t encounter_crypto_openssl_qInv(encounter_t *ctx, \
	BIGNUM *qInv, const BIGNUM *p, const BIGNUM *q, BN_CTX *bnctx)

{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
        if (!qInv || !p || !q || !bnctx) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM,
                        "null param");
                goto end;
        }

	if (!BN_mod(qInv, q, p, bnctx)) OPENSSL_ERROR(end);
	if (!BN_mod_inverse(qInv, qInv, p, bnctx)) OPENSSL_ERROR(end);

	ctx->rc = ENCOUNTER_OK;

end:
	return ctx->rc;
}

static encounter_err_t encounter_crypto_openssl_invMod2toW(\
	encounter_t *ctx, BIGNUM *ninvmod2tow, const BIGNUM *n, \
						BN_CTX *bnctx) {
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
        if (!ninvmod2tow || !n || !bnctx) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM,
                        "null param");
        }

	BN_CTX_start(bnctx);
	BIGNUM *twotow = BN_CTX_get(bnctx);

	if (!BN_set_word(twotow, 1)) OPENSSL_ERROR(end);
	if (!BN_lshift(twotow, twotow, BN_num_bits(n))) 
		OPENSSL_ERROR(end);
	if (!BN_mod_inverse(ninvmod2tow, n, twotow, bnctx)) 
		OPENSSL_ERROR(end);

	ctx->rc = ENCOUNTER_OK;

end:
	if (twotow) BN_clear(twotow);
	if (bnctx)  BN_CTX_end(bnctx);

	return ctx->rc;
}


static encounter_err_t encounter_crypto_openssl_hConstant (\
	encounter_t *ctx, BIGNUM *hsubp, \
	const BIGNUM *g, const BIGNUM *p,const BIGNUM *psquared, \
	const BIGNUM *pinvmod2tow,BN_CTX *bnctx)
{
        if (!ctx)      
                return ENCOUNTER_ERR_PARAM;
        if (!hsubp || !g || !p || !psquared || !pinvmod2tow || !bnctx){
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM, \
                        "null param");
                goto end;
        }

	BN_CTX_start(bnctx);
	BIGNUM *tmp = BN_CTX_get(bnctx);
	BIGNUM *pmin1 = BN_CTX_get(bnctx);

	if (!pmin1)  OPENSSL_ERROR(end);

	if (!BN_sub(pmin1,p,BN_value_one())) OPENSSL_ERROR(end);
	if (!BN_mod(tmp,g,psquared,bnctx)) OPENSSL_ERROR(end);
	if (!BN_mod_exp(tmp,tmp,pmin1,psquared,bnctx)) 
		OPENSSL_ERROR(end);

	if (encounter_crypto_openssl_fastL(ctx, \
			hsubp,tmp,p,pinvmod2tow,bnctx) != ENCOUNTER_OK)
		OPENSSL_ERROR(end);
	if (!BN_mod_inverse(hsubp,hsubp,p,bnctx) ) OPENSSL_ERROR(end);
	
	ctx->rc = ENCOUNTER_OK;

end:
	if (tmp)   BN_clear(tmp);
	if (pmin1) BN_clear(pmin1);
	if (bnctx) BN_CTX_end(bnctx);

	return ctx->rc;
}

static encounter_err_t encounter_crypto_openssl_fastL(encounter_t *ctx,\
                          BIGNUM *y, const BIGNUM *u, const BIGNUM *n, \
                               const BIGNUM *ninvmod2tow, BN_CTX *bnctx)
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
        if (!y || !u || !n || !ninvmod2tow || !bnctx) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM, \
                        "null param");
                goto end;
        }

	BN_CTX_start(bnctx);
	BIGNUM *tmp = BN_CTX_get(bnctx);

	if (!BN_sub(tmp,u,BN_value_one()) ) OPENSSL_ERROR(end);

	int w = BN_num_bits(n);
	// if (!BN_mask_bits(tmp,w) ) OPENSSL_ERROR(end); 
	BN_mask_bits(tmp,w);
	if (! BN_mul(y,tmp,ninvmod2tow,bnctx) ) OPENSSL_ERROR(end);
        // if (!BN_mask_bits(y,w) ) OPENSSL_ERROR(end); 
	BN_mask_bits(y,w);

	ctx->rc = ENCOUNTER_OK;

end:
	if (tmp)   BN_clear(tmp);
	if (bnctx) BN_CTX_end(bnctx);

	return ctx->rc;
}


encounter_err_t encounter_crypto_openssl_new_counter(encounter_t *ctx, ec_keyctx_t *pubK, ec_count_t **counter) 
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;

	if (counter) {
		*counter = calloc(1, sizeof **counter);
		if (*counter) {
			(*counter)->version = ENCOUNTER_COUNT_PAILLIER_V1;
			(*counter)->c = BN_new();

			encounter_crypto_openssl_paillierEncrypt(\
				ctx, (*counter)->c, ctx->m, pubK);
                        if (ctx->rc == ENCOUNTER_OK) {
			        /* Update the time of last modification */
			        time(&((*counter)->lastUpdated));
                        } else {
                                free(*counter);
                                *counter = NULL;
                        }
		} else
			ctx->rc = ENCOUNTER_ERR_MEM;
		
		return ctx->rc;
	}

	ctx->rc = ENCOUNTER_ERR_PARAM;
	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_free_counter(encounter_t *ctx, ec_count_t *counter_p) 
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;

	if (counter_p) {
		BN_free(counter_p->c);
		memset(counter_p, 0, sizeof *counter_p);

	} else ctx->rc = ENCOUNTER_ERR_PARAM;

	return ctx->rc;
}

static encounter_err_t encounter_crypto_openssl_paillierEncrypt(\
  encounter_t *ctx, BIGNUM *c, const BIGNUM *m, const ec_keyctx_t *pubK)
{
        if (!ctx)               return ENCOUNTER_ERR_PARAM;
        if (!c || !m || !pubK)  {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM, \
                        "null param");
                goto end;
        }

	BN_CTX *bnctx = BN_CTX_new();
	bool in = false;
	BN_CTX_start(bnctx);
	BIGNUM *tmp   = BN_CTX_get(bnctx);
	BIGNUM *tmp2  = BN_CTX_get(bnctx);
	BIGNUM *r     = BN_CTX_get(bnctx);


	if (!BN_mod_exp(tmp, pubK->k.paillier_pubK.g, m, \
			pubK->k.paillier_pubK.nsquared, bnctx))
		OPENSSL_ERROR(end);

	for (;;)
   	{
   		if (!BN_rand_range(r, pubK->k.paillier_pubK.n))
			OPENSSL_ERROR(end);
   		if (IsInZnstar(ctx, r,pubK->k.paillier_pubK.n, \
				bnctx, &in) != ENCOUNTER_OK) {
			OPENSSL_ERROR(end); 
		}
		if (in == true) break;
   	}

	if (!BN_mod_exp(tmp2, r, pubK->k.paillier_pubK.n, \
		pubK->k.paillier_pubK.nsquared, bnctx))
		OPENSSL_ERROR(end);
	if (!BN_mod_mul(c, tmp, tmp2, \
			pubK->k.paillier_pubK.nsquared, bnctx))
		OPENSSL_ERROR(end);

	ctx->rc = ENCOUNTER_OK;

end:
	if (tmp)   BN_clear(tmp); 
	if (tmp2)  BN_clear(tmp2); 
	if (r)     BN_clear(r);
	if (bnctx) BN_CTX_end(bnctx);
	if (bnctx) BN_CTX_free(bnctx);

	return ctx->rc;
}

static encounter_err_t IsInZnstar(encounter_t *ctx, const BIGNUM *a,\
		const BIGNUM *n, BN_CTX *bnctx, bool *in)
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
	if (!a || !n || !bnctx || !in) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM, \
                        "null param");
                goto end;
        }

	*in = true;
	ctx->rc = ENCOUNTER_OK;

	if (BN_cmp(a,n) >= 0) goto end;

	BN_CTX_start(bnctx);
	BIGNUM *tmp = BN_CTX_get(bnctx);

	if (!tmp) OPENSSL_ERROR(end);

	if (!BN_gcd(tmp,a,n,bnctx)) OPENSSL_ERROR(end);
	if (BN_is_not_one(tmp)) *in = false;


end:
	if (tmp)   BN_clear(tmp);
	if (bnctx) BN_CTX_end(bnctx);

	return ctx->rc;
}

static encounter_err_t IsInZnSquaredstar(encounter_t *ctx, \
    const BIGNUM *a, const BIGNUM *nsquared, BN_CTX *bnctx, bool *in)
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
	if (!a || !nsquared || !bnctx || !in)  {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM, \
                        "null param");
                goto end;
        }

	*in = false;
	ctx->rc = ENCOUNTER_OK;

	if (BN_cmp(a,nsquared) >= 0) goto end;

	BN_CTX_start(bnctx);
	BIGNUM *tmp = BN_CTX_get(bnctx);

	if (!tmp) OPENSSL_ERROR(end);

	/* a is in Z*_n^2 iff GCD(a, n^2) = 1 */
	if (!BN_gcd(tmp,a,nsquared,bnctx)) OPENSSL_ERROR(end);
	if (BN_is_one(tmp)) *in = true;

end:
	if (tmp) BN_clear(tmp);
	if (bnctx) BN_CTX_end(bnctx);

	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_inc(encounter_t *ctx, \
	ec_count_t *counter, ec_keyctx_t *pubK, const unsigned int a) 
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
        if (!counter || !pubK) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM, \
                        "null param");
                goto end;
        }
	BN_CTX *bnctx = BN_CTX_new();

	if (encounter_crypto_openssl_paillierUpdate(ctx, \
		counter->c, pubK, bnctx, a, false) != ENCOUNTER_OK)
		OPENSSL_ERROR(end);
			
	ctx->rc = ENCOUNTER_OK;

end:
	/* Update the time of last modification */
	time(&(counter->lastUpdated));

	if (bnctx) BN_CTX_free(bnctx);
	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_dec(encounter_t *ctx, \
         ec_count_t *counter, ec_keyctx_t *pubK, const unsigned int a) 
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
        if (!counter || !pubK) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM, \
                        "null param");
                goto end;
        }
	BN_CTX *bnctx = BN_CTX_new();

	if (encounter_crypto_openssl_paillierUpdate(ctx, \
		counter->c, pubK, bnctx, a, true) != ENCOUNTER_OK)
		OPENSSL_ERROR(end);
			
	ctx->rc = ENCOUNTER_OK;

end:
	/* Update the time of last modification */
	time(&(counter->lastUpdated));

	if (bnctx) BN_CTX_free(bnctx);
	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_mul(encounter_t *ctx, \
         ec_count_t *counter, ec_keyctx_t *pubK, const unsigned int a)
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
        if (!counter || !pubK) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM, \
                        "null param");
                goto end;
        }
	BN_CTX *bnctx = BN_CTX_new();

	if (encounter_crypto_openssl_paillierMul(ctx, \
		counter->c, pubK, bnctx, a, false) != ENCOUNTER_OK)
		OPENSSL_ERROR(end);
			
	ctx->rc = ENCOUNTER_OK;

end:
	/* Update the time of last modification */
	time(&(counter->lastUpdated));

	if (bnctx) BN_CTX_free(bnctx);
	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_mul_rand(encounter_t *ctx, \
                                ec_count_t *counter, ec_keyctx_t *pubK)
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
        if (!counter || !pubK) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM, \
                        "null param");
                goto end;
        }
	BN_CTX *bnctx = BN_CTX_new();

	if (encounter_crypto_openssl_paillierMul(ctx, \
		counter->c, pubK, bnctx, 0, true) != ENCOUNTER_OK)
		OPENSSL_ERROR(end);
			
	ctx->rc = ENCOUNTER_OK;

end:
	/* Update the time of last modification */
	time(&(counter->lastUpdated));

	if (bnctx) BN_CTX_free(bnctx);
	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_dup(encounter_t *ctx, \
                ec_keyctx_t *pubK, ec_count_t *from, ec_count_t **to)
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
	if (from && to) {
		*to = calloc(1, sizeof **to);
		if (*to) {
			(*to)->version = from->version;
			(*to)->c = BN_dup(from->c);
                        if ((*to)->c == NULL) {
                                encounter_set_error(ctx, \
                                   ENCOUNTER_ERR_MEM, "openssl: %s",\
                                   ERR_error_string(ERR_get_error(),\
                                   NULL));
                                free(*to);
                                *to = NULL;
                                return ctx->rc;
                        }

                        encounter_crypto_openssl_touch(ctx, *to, pubK);
                        if (ctx->rc == ENCOUNTER_OK) {
			        /* Update the time of last modification */
			        time(&((*to)->lastUpdated));
                        } else {
                                free(*to);
                                *to = NULL;
                        }
		} else
			ctx->rc = ENCOUNTER_ERR_MEM;
		
		return ctx->rc;
	}

	ctx->rc = ENCOUNTER_ERR_PARAM;
	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_copy(encounter_t *ctx, \
                ec_keyctx_t *pubK, ec_count_t *from, ec_count_t *to)
{
        if (from && to) {
                to->version = from->version;
                if (!BN_copy(to->c, from->c)) {
                        encounter_set_error(ctx, ENCOUNTER_ERR_MEM, \
                                "openssl: %s", ERR_error_string( \
                                ERR_get_error(), NULL));
                        return ctx->rc;
                }


                /* we are returning the return code from touch() */
                encounter_crypto_openssl_touch(ctx, to, pubK);
                time(&(to->lastUpdated));

        } else  ctx->rc = ENCOUNTER_ERR_PARAM;

        return ctx->rc;
}

/* encounter_cmp() naive (and straightforward) implementation */
encounter_err_t encounter_crypto_openssl_cmp(encounter_t *ctx, \
                ec_count_t *a, ec_count_t *b, ec_keyctx_t *privKA,\
                                  ec_keyctx_t *privKB, int *result)
{
        unsigned long long int pa, pb;
        ec_keyctx_t *ka, *kb;

        if (!ctx)       return ENCOUNTER_ERR_PARAM;
        if (!a || !b || !result) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM,
                        "null param");
                goto end;
        }

        /* At least one private key must be supplied */
        if (!privKA && !privKB) goto end;

        /* Decrypt each counter with the respective private-key,
         * if available. Otherwise, use the only supplied key to
         * decrypt both */
        ka = (privKA ? privKA : privKB);
        kb = (privKB ? privKB : privKA);

        if (encounter_crypto_openssl_decrypt(ctx, a, ka, &pa)
                != ENCOUNTER_OK) OPENSSL_ERROR(end);
        if (encounter_crypto_openssl_decrypt(ctx, b, kb, &pb)
                != ENCOUNTER_OK) OPENSSL_ERROR(end);

             if (pa  < pb) *result = -1;
        else if (pa == pb) *result =  0;
        else               *result =  1;

        ctx->rc = ENCOUNTER_OK;
end:
        pa = 0; pb = 0;

        return ctx->rc;
}

/** encounter_private_cmp2()
 * Compares the supplied counters encrypted under a common public-key
 * without ever decrypting the same counters. encounter_private_cmp2()
 * can use the supplied private-key to decrypt a quantity derived from
 * the cryptographic counters and hard to reverse-engineer.
 * The result is -1 if a < b, 0 if a == b and 1 if a > b */
encounter_err_t encounter_crypto_openssl_private_cmp2(encounter_t *ctx, \
                                 ec_count_t *a, ec_count_t *b, \
                      ec_keyctx_t *pubK,ec_keyctx_t *privK, int *result)
{
        if (!ctx) return ENCOUNTER_ERR_PARAM;
        if (!a || !b || !pubK || !privK || !result) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM, \
                        "null param");
                goto end;
        }

        encounter_err_t rc;
        unsigned long long int c;
        bool in = false;
        ec_count_t *diffAB = NULL;
	BN_CTX *bnctx = BN_CTX_new();
	BIGNUM *rand  = BN_CTX_get(bnctx);
	BIGNUM *tmp   = BN_CTX_get(bnctx);
	BIGNUM *tmp2  = BN_CTX_get(bnctx);
	BIGNUM *r     = BN_CTX_get(bnctx);
	BIGNUM *m     = BN_CTX_get(bnctx);
        BIGNUM *pmin1 = BN_CTX_get(bnctx); 
	BIGNUM *qmin1 = BN_CTX_get(bnctx);
	BIGNUM *msubp = BN_CTX_get(bnctx); 
        BIGNUM *msubq = BN_CTX_get(bnctx);

        if (!msubq) OPENSSL_ERROR(end);

        /* diffAB = dup a */
        if (encounter_crypto_openssl_dup(ctx, pubK, a, &diffAB) \
                != ENCOUNTER_OK) OPENSSL_ERROR(end);

        if (!BN_rand(rand, PAILLIER_RANDOMIZER_SECLEVEL + 2, 0, 1) \
                != ENCOUNTER_OK) OPENSSL_ERROR(end);

        /* Add a random delta to diffAB */
        if (!BN_mod_exp(tmp2, pubK->k.paillier_pubK.g, rand, \
                pubK->k.paillier_pubK.nsquared, bnctx))
                OPENSSL_ERROR(end);

        if (!BN_mod_mul(diffAB->c, diffAB->c, tmp2, \
                        pubK->k.paillier_pubK.nsquared, bnctx) )
                OPENSSL_ERROR(end);

        for (;;)
        {
                if (!BN_rand_range(r, pubK->k.paillier_pubK.n) )
                        OPENSSL_ERROR(end);
                if (IsInZnstar(ctx, r, pubK->k.paillier_pubK.n, \
                                bnctx, &in)  != ENCOUNTER_OK)
                        OPENSSL_ERROR(end);
                if (in) break;
        }
        if (!BN_mod_exp(tmp, r, pubK->k.paillier_pubK.n, \
                        pubK->k.paillier_pubK.nsquared, bnctx) )
                OPENSSL_ERROR(end);
        if (!BN_mod_mul(diffAB->c, diffAB->c, tmp, \
                pubK->k.paillier_pubK.nsquared, bnctx))
                OPENSSL_ERROR(end);
        
        /* subtract the other counter */
        if (encounter_crypto_openssl_sub(ctx, diffAB, b, pubK)
                != ENCOUNTER_OK) OPENSSL_ERROR(end);


        /* Decrypt the result */
	/* p-1 and q-1 */
	if (!BN_sub(pmin1, privK->k.paillier_privK.p, BN_value_one()))
		OPENSSL_ERROR(end);
	if (!BN_sub(qmin1, privK->k.paillier_privK.q, BN_value_one()))
		OPENSSL_ERROR(end);

	/* c^(p-1) mod p^2 */
	if (!BN_mod(tmp, diffAB->c, \
			privK->k.paillier_privK.psquared, bnctx))
		OPENSSL_ERROR(end);
	if (!BN_mod_exp(tmp, tmp, pmin1, \
			privK->k.paillier_privK.psquared, bnctx))
		OPENSSL_ERROR(end);

	/* m_p = L_p ( c^(p-1) mod p^2 ) h_p mod p */
	if (encounter_crypto_openssl_fastL(ctx, tmp, tmp, \
		privK->k.paillier_privK.p, \
		privK->k.paillier_privK.pinvmod2tow, bnctx) \
		!= ENCOUNTER_OK)
		OPENSSL_ERROR(end);

	if (!BN_mod_mul(msubp, tmp, privK->k.paillier_privK.hsubp, \
			privK->k.paillier_privK.p, bnctx))
		OPENSSL_ERROR(end);


	/* c^(q-1) */
	if (!BN_mod(tmp, diffAB->c, \
		privK->k.paillier_privK.qsquared, bnctx))
		OPENSSL_ERROR(end);
	if (!BN_mod_exp(tmp, tmp, qmin1, \
		privK->k.paillier_privK.qsquared,bnctx))
		OPENSSL_ERROR(end);

	/* m_q = L_q( c^(q-1) mod q^2 ) h_q mod q */
	if (encounter_crypto_openssl_fastL(ctx, tmp, tmp, \
			privK->k.paillier_privK.q, \
			privK->k.paillier_privK.qinvmod2tow, bnctx) \
		!= ENCOUNTER_OK)
		OPENSSL_ERROR(end);
	if (!BN_mod_mul(msubq, tmp, privK->k.paillier_privK.hsubq, \
			privK->k.paillier_privK.q, bnctx))
		OPENSSL_ERROR(end);

	/* m = CRT(m_p, m_q) mod pq */
	if (encounter_crypto_openssl_fastCRT(ctx, m, msubp, \
			privK->k.paillier_privK.p, msubq, \
			privK->k.paillier_privK.q, \
			privK->k.paillier_privK.qInv, bnctx) \
		!= ENCOUNTER_OK)
		OPENSSL_ERROR(end);

        /* Compare */
        *result = BN_cmp(m, rand);

        /* We are done */
	ctx->rc = ENCOUNTER_OK;

end:
        c = 0;
        if (diffAB) encounter_crypto_openssl_free_counter(ctx, diffAB);
        if (rand)   BN_clear(rand); 
        if (tmp)    BN_clear(tmp);
        if (tmp2)   BN_clear(tmp2);
        if (r)      BN_clear(r);
	if (pmin1)  BN_clear(pmin1); 
	if (qmin1)  BN_clear(qmin1);
	if (msubp)  BN_clear(msubp); 
	if (msubq)  BN_clear(msubq);
	if (m)      BN_clear(m);
        if (bnctx)  BN_CTX_free(bnctx);
        return ctx->rc;
}

static encounter_err_t encounter_crypto_openssl_paillierUpdate(\
  encounter_t *ctx, BIGNUM *c, const ec_keyctx_t *pubK, BN_CTX *bnctx, \
			const unsigned int amount, bool decrement)
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
	if (!c || !pubK || !bnctx) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM, \
                        "null param");
                goto end;
        }

	BN_CTX_start(bnctx);
	BIGNUM *tmp = BN_CTX_get(bnctx);
	BIGNUM *tmp2 = BN_CTX_get(bnctx);
	BIGNUM *r = BN_CTX_get(bnctx);
	BIGNUM *m = BN_CTX_get(bnctx);
	bool	in = false;

	if (!m) OPENSSL_ERROR(end);
	if (!BN_set_word(m, amount)) OPENSSL_ERROR(end);

#if 0
	fprintf(stdout, "paillier inc: before increment: ");
	BN_print_fp(stdout, c);
	fprintf(stdout, "\n");
#endif
	if (amount == 1)  {
		/* monotonically increasing/decreasing */
		if (!BN_copy(tmp2, pubK->k.paillier_pubK.g))
			OPENSSL_ERROR(end);
	} else {
		/* increment/decrement by the given amount */
		if (!BN_mod_exp(tmp2, pubK->k.paillier_pubK.g, m, \
			pubK->k.paillier_pubK.nsquared, bnctx))
			OPENSSL_ERROR(end);
	}


	if (decrement) /* decrementing */
		if (!BN_mod_inverse(tmp2, tmp2, \
			pubK->k.paillier_pubK.nsquared, bnctx) )
			OPENSSL_ERROR(end);

	if (!BN_mod_mul(c, c, tmp2, \
			pubK->k.paillier_pubK.nsquared, bnctx) )  
		OPENSSL_ERROR(end);
	
	for (;;)
   	{
		if (!BN_rand_range(r, pubK->k.paillier_pubK.n) )
			OPENSSL_ERROR(end);
   		if (IsInZnstar(ctx, r, pubK->k.paillier_pubK.n, \
				bnctx, &in)  != ENCOUNTER_OK)
			OPENSSL_ERROR(end);
		if (in) break;
   	}
	if (!BN_mod_exp(tmp, r, pubK->k.paillier_pubK.n, \
			pubK->k.paillier_pubK.nsquared, bnctx) ) 
		OPENSSL_ERROR(end);
	if (!BN_mod_mul(c, c, tmp, pubK->k.paillier_pubK.nsquared, bnctx))
		OPENSSL_ERROR(end);

#if 0
	fprintf(stdout, "paillier inc: after incrementing: ");
	BN_print_fp(stdout, c);
	fprintf(stdout, "\n");
#endif
	ctx->rc = ENCOUNTER_OK;

end:
	if (tmp)   BN_clear(tmp); 
	if (tmp2)  BN_clear(tmp2); 
	if (r)     BN_clear(r);
	if (m)     BN_clear(m);
	if (bnctx) BN_CTX_end(bnctx);

	return ctx->rc;
}

static encounter_err_t encounter_crypto_openssl_paillierMul(\
  encounter_t *ctx, BIGNUM *c, const ec_keyctx_t *pubK, BN_CTX *bnctx,\
	unsigned int amount, bool rand)
{

        if (!ctx)       return ENCOUNTER_ERR_PARAM;
	if (!c || !pubK || !bnctx) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM,
                        "null param");
                goto end;
        }

	BN_CTX_start(bnctx);
	BIGNUM *tmp = BN_CTX_get(bnctx);
	BIGNUM *r = BN_CTX_get(bnctx);
	BIGNUM *m = BN_CTX_get(bnctx);
	bool	in = false;

	if (!m) OPENSSL_ERROR(end);
        if (rand) {
                if (!BN_rand(m, PAILLIER_RANDOMIZER_SECLEVEL + 2, 0, 1)
                        != ENCOUNTER_OK) OPENSSL_ERROR(end);
        } else  {
	        if (!BN_set_word(m, amount)) OPENSSL_ERROR(end);
        }

#if 0
	fprintf(stdout, "paillier inc: before increment: ");
	BN_print_fp(stdout, c);
	fprintf(stdout, "\n");
#endif
	if (!BN_mod_exp(c, c, m, \
			pubK->k.paillier_pubK.nsquared, bnctx))
		OPENSSL_ERROR(end);
	
	for (;;)
   	{
		if (!BN_rand_range(r, pubK->k.paillier_pubK.n) )
			OPENSSL_ERROR(end);
   		if (IsInZnstar(ctx, r, pubK->k.paillier_pubK.n, \
				bnctx, &in)  != ENCOUNTER_OK)
			OPENSSL_ERROR(end);
		if (in) break;
   	}
	if (!BN_mod_exp(tmp, r, pubK->k.paillier_pubK.n, \
			pubK->k.paillier_pubK.nsquared, bnctx) ) 
		OPENSSL_ERROR(end);
	if (!BN_mod_mul(c, c, tmp, pubK->k.paillier_pubK.nsquared, bnctx))
		OPENSSL_ERROR(end);

#if 0
	fprintf(stdout, "paillier inc: after incrementing: ");
	BN_print_fp(stdout, c);
	fprintf(stdout, "\n");
#endif
	ctx->rc = ENCOUNTER_OK;

end:
	if (tmp)  BN_clear(tmp); 
	if (r)    BN_clear(r);
	if (m)    BN_clear(m);
	if (bnctx) BN_CTX_end(bnctx);

	return ctx->rc;
}



encounter_err_t encounter_crypto_openssl_touch(encounter_t *ctx, \
			ec_count_t *counter, ec_keyctx_t *pubK) 
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
        if (!counter || !pubK) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM,
                        "null param");
                goto end;
        }
	BN_CTX *bnctx = BN_CTX_new();
	BN_CTX_start(bnctx);
	BIGNUM *tmp = BN_CTX_get(bnctx);
	BIGNUM *r = BN_CTX_get(bnctx);
	bool	in = false;

	if (!r)	OPENSSL_ERROR(end);

	for (;;)
   	{
   		if (!BN_rand_range(r, pubK->k.paillier_pubK.n))
			OPENSSL_ERROR(end);
   		if (IsInZnstar(ctx, r,pubK->k.paillier_pubK.n, \
				bnctx, &in) != ENCOUNTER_OK)
			OPENSSL_ERROR(end);
		if (in) break; 
   	}
	if (!BN_mod_exp(tmp, r, pubK->k.paillier_pubK.n, \
			pubK->k.paillier_pubK.nsquared, bnctx))
		OPENSSL_ERROR(end);
	if (!BN_mod_mul(counter->c, counter->c, tmp, \
			pubK->k.paillier_pubK.nsquared, bnctx))
		OPENSSL_ERROR(end);

	/* Update the time of last modification */
	time(&(counter->lastUpdated));

	
	ctx->rc = ENCOUNTER_OK;

end:
	if (tmp)   BN_clear(tmp);
	if (r)	   BN_clear(r);
	if (bnctx) BN_CTX_end(bnctx);
	if (bnctx) BN_CTX_free(bnctx);

	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_add(encounter_t *ctx, \
       ec_count_t *encountA, ec_count_t *encountB, ec_keyctx_t *pubK)
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
        if (!encountA || !encountB || !pubK) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM,
                        "null param");
                goto end;
        }
	BN_CTX *bnctx = BN_CTX_new();

	if (encounter_crypto_openssl_paillierAddSub(ctx, \
             encountA->c, encountB->c, pubK, bnctx, false) \
             != ENCOUNTER_OK)
		OPENSSL_ERROR(end);
			
	ctx->rc = ENCOUNTER_OK;

end:
	/* Update the time of last modification */
	time(&(encountA->lastUpdated));

	if (bnctx) BN_CTX_free(bnctx);
	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_sub(encounter_t *ctx, \
       ec_count_t *encountA, ec_count_t *encountB, ec_keyctx_t *pubK)
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
        if (!encountA || !encountB || !pubK) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM,
                        "null param");
                goto end;
        }
	BN_CTX *bnctx = BN_CTX_new();

	if (encounter_crypto_openssl_paillierAddSub(ctx, \
             encountA->c, encountB->c, pubK, bnctx, true) \
             != ENCOUNTER_OK)
		OPENSSL_ERROR(end);
			
	ctx->rc = ENCOUNTER_OK;

end:
	/* Update the time of last modification */
	time(&(encountA->lastUpdated));

	if (bnctx) BN_CTX_free(bnctx);
	return ctx->rc;
}

static encounter_err_t encounter_crypto_openssl_paillierAddSub(  \
    		encounter_t *ctx, BIGNUM *c, BIGNUM *b, \
	const ec_keyctx_t *pubK, BN_CTX *bnctx, const bool subtract)
{
	if (!ctx) return ENCOUNTER_ERR_PARAM;
	if (!c || !b || !pubK || !bnctx) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM, \
                        "null param");
                goto end;
        }

	BN_CTX_start(bnctx);
	BIGNUM *tmp = BN_CTX_get(bnctx);
	BIGNUM *tmp2 = BN_CTX_get(bnctx);
	BIGNUM *r = BN_CTX_get(bnctx);
	bool in = false;

	if (!r) OPENSSL_ERROR(end);

#if 0
	fprintf(stdout, "paillier inc: before increment: ");
	BN_print_fp(stdout, c);
	fprintf(stdout, "\n");
#endif

        if (subtract) {
                /* FIXME: prevent from decrementing below zero */
		if (!BN_mod_inverse(tmp2, b, \
			pubK->k.paillier_pubK.nsquared, bnctx) )
			OPENSSL_ERROR(end);
        } else {
                if (!BN_copy(tmp2, b))
                        OPENSSL_ERROR(end);
        }

	if (!BN_mod_mul(c, c, tmp2, \
			pubK->k.paillier_pubK.nsquared, bnctx) )  
		OPENSSL_ERROR(end);
	
	for (;;)
   	{
		if (!BN_rand_range(r, pubK->k.paillier_pubK.n) )
			OPENSSL_ERROR(end);
   		if (IsInZnstar(ctx, r, pubK->k.paillier_pubK.n, \
				bnctx, &in)  != ENCOUNTER_OK)
			OPENSSL_ERROR(end);
		if (in) break;
   	}
	if (!BN_mod_exp(tmp, r, pubK->k.paillier_pubK.n, \
			pubK->k.paillier_pubK.nsquared, bnctx) ) 
		OPENSSL_ERROR(end);
	if (!BN_mod_mul(c, c, tmp, pubK->k.paillier_pubK.nsquared, bnctx))
		OPENSSL_ERROR(end);

#if 0
	fprintf(stdout, "paillier inc: after incrementing: ");
	BN_print_fp(stdout, c);
	fprintf(stdout, "\n");
#endif
	ctx->rc = ENCOUNTER_OK;

end:
	if (tmp)   BN_clear(tmp); 
	if (tmp2)  BN_clear(tmp); 
	if (r)     BN_clear(r);
	if (bnctx) BN_CTX_end(bnctx);

	return ctx->rc;

}

encounter_err_t encounter_crypto_openssl_decrypt(encounter_t *ctx, \
     ec_count_t *counter, ec_keyctx_t *privK, unsigned long long int *a)
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;

	if ( !counter || !privK ||  !a) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM, \
                        "null param");
                goto end;
        }

	BN_CTX *bnctx = BN_CTX_new();
	BN_CTX_start(bnctx);
	BIGNUM *m = BN_CTX_get(bnctx);

	BIGNUM *tmp, *pmin1, *qmin1, *msubp, *msubq;

	tmp = BN_CTX_get(bnctx); pmin1 = BN_CTX_get(bnctx); 
	qmin1 = BN_CTX_get(bnctx);
	msubp = BN_CTX_get(bnctx); msubq = BN_CTX_get(bnctx);

	if (!msubq) OPENSSL_ERROR(end);

	/* p-1 and q-1 */
	if (!BN_sub(pmin1, privK->k.paillier_privK.p, BN_value_one()))
		OPENSSL_ERROR(end);
	if (!BN_sub(qmin1, privK->k.paillier_privK.q, BN_value_one()))
		OPENSSL_ERROR(end);

	/* c^(p-1) mod p^2 */
	if (!BN_mod(tmp, counter->c, \
			privK->k.paillier_privK.psquared, bnctx))
		OPENSSL_ERROR(end);
	if (!BN_mod_exp(tmp, tmp, pmin1, \
			privK->k.paillier_privK.psquared, bnctx))
		OPENSSL_ERROR(end);

	/* m_p = L_p ( c^(p-1) mod p^2 ) h_p mod p */
	if (encounter_crypto_openssl_fastL(ctx, tmp, tmp, \
		privK->k.paillier_privK.p, \
		privK->k.paillier_privK.pinvmod2tow, bnctx) \
		!= ENCOUNTER_OK)
		OPENSSL_ERROR(end);

	if (!BN_mod_mul(msubp, tmp, privK->k.paillier_privK.hsubp, \
			privK->k.paillier_privK.p, bnctx))
		OPENSSL_ERROR(end);


	/* c^(q-1) */
	if (!BN_mod(tmp, counter->c, \
		privK->k.paillier_privK.qsquared, bnctx))
		OPENSSL_ERROR(end);
	if (!BN_mod_exp(tmp, tmp, qmin1, \
		privK->k.paillier_privK.qsquared,bnctx))
		OPENSSL_ERROR(end);

	/* m_q = L_q( c^(q-1) mod q^2 ) h_q mod q */
	if (encounter_crypto_openssl_fastL(ctx, tmp, tmp, \
			privK->k.paillier_privK.q, \
			privK->k.paillier_privK.qinvmod2tow, bnctx) \
		!= ENCOUNTER_OK)
		OPENSSL_ERROR(end);
	if (!BN_mod_mul(msubq, tmp, privK->k.paillier_privK.hsubq, \
			privK->k.paillier_privK.q, bnctx))
		OPENSSL_ERROR(end);

	/* m = CRT(m_p, m_q) mod pq */
	if (encounter_crypto_openssl_fastCRT(ctx, m, msubp, \
			privK->k.paillier_privK.p, msubq, \
			privK->k.paillier_privK.q, \
			privK->k.paillier_privK.qInv, bnctx) \
		!= ENCOUNTER_OK)
		OPENSSL_ERROR(end);

	/* Make the plaintext counter available via a */
	char *plainC = BN_bn2dec(m);
	if (!plainC) OPENSSL_ERROR(end);

	*a = strtoull(plainC, NULL, 10);
	OPENSSL_free(plainC);
        if (*a == ULLONG_MAX) {
                encounter_set_error(ctx, ENCOUNTER_ERR_OVERFLOW, \
                    "The requested value is larger than ULLONG_MAX. ");
                goto end;
        }

        /* We are done */
	ctx->rc = ENCOUNTER_OK;

end:
	if (tmp)   BN_clear(tmp); 
	if (pmin1) BN_clear(pmin1); 
	if (qmin1) BN_clear(qmin1);
	if (msubp) BN_clear(msubp); 
	if (msubq) BN_clear(msubq);
	if (m)     BN_clear(m);

	if (bnctx) BN_CTX_end(bnctx);
	if (bnctx) BN_CTX_free(bnctx);

	return ctx->rc;
}


static encounter_err_t encounter_crypto_openssl_fastCRT(\
	encounter_t *ctx, BIGNUM *g, const BIGNUM *g1, const BIGNUM *p,\
   const BIGNUM *g2, const BIGNUM *q, const BIGNUM *qInv, BN_CTX *bnctx)
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
	if (!g || !g1 || !p || !g2 || !q || !qInv || !bnctx) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM, \
                        "null param");
		goto end;
        }

	BN_CTX_start(bnctx);
	BIGNUM *tmp = BN_CTX_get(bnctx);
	BIGNUM *h = BN_CTX_get(bnctx);

	if (!h) OPENSSL_ERROR(end);

	if (!BN_sub(tmp,g1,g2)) OPENSSL_ERROR(end);
	if (BN_is_neg(tmp))
   		if (!BN_add(tmp,tmp,p)) OPENSSL_ERROR(end);

	if (!BN_mod_mul(h,tmp,qInv,p, bnctx)) OPENSSL_ERROR(end);
	if (!BN_mul(tmp,q,h, bnctx)) OPENSSL_ERROR(end);
	if (!BN_add(g,g2,tmp)) OPENSSL_ERROR(end);

	ctx->rc = ENCOUNTER_OK;

end:
	if (tmp)   BN_clear(tmp);
	if (tmp)   BN_clear(h);
	if (bnctx) BN_CTX_end(bnctx);

	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_numToString(encounter_t  *ctx,\
                ec_keyctx_t *keyctx, ec_keystring_t **key) 
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
	if (keyctx && key) {
		*key = calloc(1, sizeof **key);
		if (*key == NULL) {
			ctx->rc = ENCOUNTER_ERR_MEM;
			return ctx->rc;
		}
		/* The keytype maps to itself */
		(*key)->type = keyctx->type;

		/* Set the key components in hex form */
		switch (keyctx->type) {
			case EC_KEYTYPE_PAILLIER_PUBLIC:
				(*key)->k.paillier_pubK.n = \
				BN_bn2hex(keyctx->k.paillier_pubK.n);

				(*key)->k.paillier_pubK.g = \
				BN_bn2hex(keyctx->k.paillier_pubK.g);

				(*key)->k.paillier_pubK.nsquared = \
				BN_bn2hex(keyctx->k.paillier_pubK.nsquared);

				if (  (*key)->k.paillier_pubK.n 
				    &&(*key)->k.paillier_pubK.g
				    &&(*key)->k.paillier_pubK.nsquared)
					ctx->rc = ENCOUNTER_OK;
				else	ctx->rc = ENCOUNTER_ERR_CRYPTO;

				break;

			case EC_KEYTYPE_PAILLIER_PRIVATE:
				(*key)->k.paillier_privK.p = \
				BN_bn2hex(keyctx->k.paillier_privK.p);

				(*key)->k.paillier_privK.q = \
				BN_bn2hex(keyctx->k.paillier_privK.q);

				(*key)->k.paillier_privK.psquared = \
				BN_bn2hex(keyctx->k.paillier_privK.psquared);

				(*key)->k.paillier_privK.qsquared = \
				BN_bn2hex(keyctx->k.paillier_privK.qsquared);

				(*key)->k.paillier_privK.pinvmod2tow = \
				BN_bn2hex(keyctx->k.paillier_privK.pinvmod2tow);

				(*key)->k.paillier_privK.qinvmod2tow = \
				BN_bn2hex(keyctx->k.paillier_privK.qinvmod2tow);

				(*key)->k.paillier_privK.hsubp = \
				BN_bn2hex(keyctx->k.paillier_privK.hsubp);

				(*key)->k.paillier_privK.hsubq = \
				BN_bn2hex(keyctx->k.paillier_privK.hsubq);

				(*key)->k.paillier_privK.qInv = \
				BN_bn2hex(keyctx->k.paillier_privK.qInv);
				
				if (  (*key)->k.paillier_privK.p
				    &&(*key)->k.paillier_privK.q
				    &&(*key)->k.paillier_privK.psquared
				    &&(*key)->k.paillier_privK.qsquared
				    &&(*key)->k.paillier_privK.pinvmod2tow
				    &&(*key)->k.paillier_privK.qinvmod2tow
				    &&(*key)->k.paillier_privK.hsubp
				    &&(*key)->k.paillier_privK.hsubq
				    &&(*key)->k.paillier_privK.qInv)
					ctx->rc = ENCOUNTER_OK;
				else	ctx->rc = ENCOUNTER_ERR_CRYPTO;

				break;

			default:
				assert(NOTREACHED);
				break;
		}
	} else ctx->rc = ENCOUNTER_ERR_PARAM;

	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_stringToNum(encounter_t *ctx,\
                ec_keystring_t *key, ec_keyctx_t **keyctx) 

{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
	if (key && keyctx) {
		switch (key->type) {
			case EC_KEYTYPE_PAILLIER_PUBLIC:
				if (encounter_crypto_openssl_new_keyctx(\
				    EC_KEYTYPE_PAILLIER_PUBLIC ,keyctx )\
				 != ENCOUNTER_OK) 
					break;
				
				(*keyctx)->type = key->type;				
				BN_hex2bn(&(*keyctx)->k.paillier_pubK.n, \
					key->k.paillier_pubK.n);

				BN_hex2bn(&(*keyctx)->k.paillier_pubK.g, \
					key->k.paillier_pubK.g);

				BN_hex2bn(\
				&(*keyctx)->k.paillier_pubK.nsquared, \
				key->k.paillier_pubK.nsquared);

				if (  (*keyctx)->k.paillier_pubK.n 
				    &&(*keyctx)->k.paillier_pubK.g
				    &&(*keyctx)->k.paillier_pubK.nsquared)
					ctx->rc = ENCOUNTER_OK;
				else	ctx->rc = ENCOUNTER_ERR_CRYPTO;

				break;

			case EC_KEYTYPE_PAILLIER_PRIVATE:
				if (encounter_crypto_openssl_new_keyctx(\
				    EC_KEYTYPE_PAILLIER_PRIVATE ,keyctx )\
				 != ENCOUNTER_OK) 
					break;

				(*keyctx)->type = key->type;				

				BN_hex2bn(&(*keyctx)->k.paillier_privK.p,\
					key->k.paillier_privK.p);

				BN_hex2bn(&(*keyctx)->k.paillier_privK.q,\
					key->k.paillier_privK.q);

				BN_hex2bn(\
				&(*keyctx)->k.paillier_privK.psquared,
				key->k.paillier_privK.psquared);

				BN_hex2bn(\
				&(*keyctx)->k.paillier_privK.qsquared,
				key->k.paillier_privK.qsquared);

				BN_hex2bn(\
				&(*keyctx)->k.paillier_privK.pinvmod2tow,\
				key->k.paillier_privK.pinvmod2tow);

				BN_hex2bn(\
				&(*keyctx)->k.paillier_privK.qinvmod2tow,
				key->k.paillier_privK.qinvmod2tow);

				BN_hex2bn(\
				&(*keyctx)->k.paillier_privK.hsubp,
				key->k.paillier_privK.hsubp);

				BN_hex2bn(\
				&(*keyctx)->k.paillier_privK.hsubq,
				key->k.paillier_privK.hsubq);

				BN_hex2bn(\
				&(*keyctx)->k.paillier_privK.qInv,
				key->k.paillier_privK.qInv);

				if (  (*keyctx)->k.paillier_privK.p
				    &&(*keyctx)->k.paillier_privK.q
				    &&(*keyctx)->k.paillier_privK.psquared
				    &&(*keyctx)->k.paillier_privK.qsquared
				    &&(*keyctx)->k.paillier_privK.pinvmod2tow
				    &&(*keyctx)->k.paillier_privK.qinvmod2tow
				    &&(*keyctx)->k.paillier_privK.hsubp
				    &&(*keyctx)->k.paillier_privK.hsubq
				    &&(*keyctx)->k.paillier_privK.qInv)
					ctx->rc = ENCOUNTER_OK;
				else	ctx->rc = ENCOUNTER_ERR_CRYPTO;
				break;

			default:
				ctx->rc = ENCOUNTER_ERR_DATA;
				break;
		}
	} else ctx->rc = ENCOUNTER_ERR_PARAM;

	/* We are done */
	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_counterToString(\
	encounter_t *ctx, ec_count_t *encount, char **counter) 
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;

	if (encount && counter) {
		*counter = BN_bn2hex(encount->c);
		if (counter) 	ctx->rc = ENCOUNTER_OK;
		else		ctx->rc = ENCOUNTER_ERR_CRYPTO;

	} else ctx->rc = ENCOUNTER_ERR_PARAM;
	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_counterStrDispose(\
				encounter_t *ctx, char *counter) 
{
	if (counter) OPENSSL_free(counter);

        ctx->rc = ENCOUNTER_OK;
        return ctx->rc;
}


encounter_err_t encounter_crypto_openssl_dispose_keystring(\
		encounter_t *ctx, ec_keystring_t *key) 
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;

	if (key) {
		switch(key->type) {
			case EC_KEYTYPE_PAILLIER_PUBLIC:
				OPENSSL_free(key->k.paillier_pubK.n);
				OPENSSL_free(key->k.paillier_pubK.g);
				OPENSSL_free(key->k.paillier_pubK.nsquared);
				memset(key, 0, sizeof *key);
				free(key);
				ctx->rc = ENCOUNTER_OK;
				break;
			case EC_KEYTYPE_PAILLIER_PRIVATE:	
				OPENSSL_free(key->k.paillier_privK.p);	
				OPENSSL_free(key->k.paillier_privK.q);	
				OPENSSL_free(key->k.paillier_privK.psquared);	
				OPENSSL_free(key->k.paillier_privK.qsquared);	
				OPENSSL_free(key->k.paillier_privK.pinvmod2tow);
				OPENSSL_free(key->k.paillier_privK.qinvmod2tow);
				OPENSSL_free(key->k.paillier_privK.hsubp);	
				OPENSSL_free(key->k.paillier_privK.hsubq);	
				OPENSSL_free(key->k.paillier_privK.qInv);
				memset(key, 0, sizeof *key);
				free(key);
				ctx->rc = ENCOUNTER_OK;
				break;
			default:	
				ctx->rc = ENCOUNTER_ERR_PARAM;	
				break;
		}
	} else ctx->rc = ENCOUNTER_ERR_PARAM;

	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_dispose_counterString(\
			encounter_t *ctx, char *counter)
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;

	if (counter) OPENSSL_free(counter);

	ctx->rc = ENCOUNTER_OK;
	return ctx->rc;
}

encounter_err_t encounter_crypto_openssl_stringToCounter(\
         encounter_t *ctx, const char *counter, ec_count_t **encount)
{
        if (!ctx)       return ENCOUNTER_ERR_PARAM;
	if (!counter || !encount) {
                encounter_set_error(ctx, ENCOUNTER_ERR_PARAM,\
                        "null param");
                goto err;
        }

	*encount = calloc(1, sizeof **encount);
	if (*encount) {
		(*encount)->version = ENCOUNTER_COUNT_PAILLIER_V1;
		if (!BN_hex2bn(&(*encount)->c, counter))
			OPENSSL_ERROR(err);

		/* Update the time of last modification */
		time(&((*encount)->lastUpdated));

		ctx->rc = ENCOUNTER_OK;
	} else  ctx->rc = ENCOUNTER_ERR_MEM;

	/* We are done */
	return ctx->rc;

err:
	if (*encount) {
		free (*encount);
		*encount = NULL;
	}
	return ctx->rc;
}
