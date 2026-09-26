// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>

#include "../cacheinfo_internal.h"

static struct cacheinfo cacheinfo_test_leaf(unsigned int level,
					    enum cache_type type,
					    unsigned int id,
					    unsigned int attributes)
{
	return (struct cacheinfo) {
		.level = level,
		.type = type,
		.id = id,
		.attributes = attributes,
	};
}

static void cacheinfo_global_id_cross_level_test(struct kunit *test)
{
	struct cacheinfo l2 = cacheinfo_test_leaf(2, CACHE_TYPE_UNIFIED, 7,
						  CACHE_ID | CACHE_ID_GLOBAL);
	struct cacheinfo l3 = cacheinfo_test_leaf(3, CACHE_TYPE_UNIFIED, 7,
						  CACHE_ID | CACHE_ID_GLOBAL);

	KUNIT_EXPECT_TRUE(test, cacheinfo_cache_leaves_are_shared(&l2, &l3));
}

static void cacheinfo_level_local_id_cross_level_test(struct kunit *test)
{
	struct cacheinfo l2 = cacheinfo_test_leaf(2, CACHE_TYPE_UNIFIED, 7,
						  CACHE_ID);
	struct cacheinfo l3 = cacheinfo_test_leaf(3, CACHE_TYPE_UNIFIED, 7,
						  CACHE_ID);

	KUNIT_EXPECT_FALSE(test, cacheinfo_cache_leaves_are_shared(&l2, &l3));
}

static void cacheinfo_global_id_mismatch_test(struct kunit *test)
{
	struct cacheinfo l2 = cacheinfo_test_leaf(2, CACHE_TYPE_UNIFIED, 7,
						  CACHE_ID | CACHE_ID_GLOBAL);
	struct cacheinfo l3 = cacheinfo_test_leaf(3, CACHE_TYPE_UNIFIED, 8,
						  CACHE_ID | CACHE_ID_GLOBAL);

	KUNIT_EXPECT_FALSE(test, cacheinfo_cache_leaves_are_shared(&l2, &l3));
}

static void cacheinfo_global_id_type_mismatch_test(struct kunit *test)
{
	struct cacheinfo data = cacheinfo_test_leaf(2, CACHE_TYPE_DATA, 7,
						    CACHE_ID | CACHE_ID_GLOBAL);
	struct cacheinfo unified = cacheinfo_test_leaf(3, CACHE_TYPE_UNIFIED, 7,
						       CACHE_ID | CACHE_ID_GLOBAL);

	KUNIT_EXPECT_FALSE(test,
			   cacheinfo_cache_leaves_are_shared(&data, &unified));
}

static void cacheinfo_one_global_id_cross_level_test(struct kunit *test)
{
	struct cacheinfo l2 = cacheinfo_test_leaf(2, CACHE_TYPE_UNIFIED, 7,
						  CACHE_ID | CACHE_ID_GLOBAL);
	struct cacheinfo l3 = cacheinfo_test_leaf(3, CACHE_TYPE_UNIFIED, 7,
						  CACHE_ID);

	KUNIT_EXPECT_FALSE(test, cacheinfo_cache_leaves_are_shared(&l2, &l3));
}

static void cacheinfo_global_flag_without_id_test(struct kunit *test)
{
	struct cacheinfo l2 = cacheinfo_test_leaf(2, CACHE_TYPE_UNIFIED, 0,
						  CACHE_ID_GLOBAL);
	struct cacheinfo l3 = cacheinfo_test_leaf(3, CACHE_TYPE_UNIFIED, 0,
						  CACHE_ID_GLOBAL);

	KUNIT_EXPECT_FALSE(test, cacheinfo_cache_leaves_are_shared(&l2, &l3));
}

static struct kunit_case cacheinfo_test_cases[] = {
	KUNIT_CASE(cacheinfo_global_id_cross_level_test),
	KUNIT_CASE(cacheinfo_level_local_id_cross_level_test),
	KUNIT_CASE(cacheinfo_global_id_mismatch_test),
	KUNIT_CASE(cacheinfo_global_id_type_mismatch_test),
	KUNIT_CASE(cacheinfo_one_global_id_cross_level_test),
	KUNIT_CASE(cacheinfo_global_flag_without_id_test),
	{}
};

static struct kunit_suite cacheinfo_test_suite = {
	.name = "cacheinfo-sharing",
	.test_cases = cacheinfo_test_cases,
};

kunit_test_suite(cacheinfo_test_suite);

MODULE_DESCRIPTION("KUnit tests for cacheinfo sharing");
MODULE_LICENSE("GPL");
