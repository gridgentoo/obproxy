/**
 * Copyright (c) 2021 OceanBase
 * OceanBase Database Proxy(ODP) is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 *
 * *************************************************************
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>
#include "all_mock.h"
#include "observer/omt/ob_cgroup_ctrl.h"
#include "lib/atomic/ob_atomic.h"

using namespace oceanbase::common;
using namespace oceanbase::omt;
using namespace oceanbase::observer;

void *thread_func(void *args)
{
  int *tid = static_cast<int *>(args);
  *tid = static_cast<int>(syscall(SYS_gettid));
  sleep(3);
}

// Create a soft chain from the current directory to the cgroup directory in advance.
// The cgroup directory needs to mount three modules: cpu, cpuset, and cpuacct
TEST(TestCgroupCtrl, AddDelete)
{
  ObCgroupCtrl cg_ctrl;
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.init());
  ASSERT_TRUE(cg_ctrl.is_valid());

  int tids[4];
  pthread_t ts[4];
  for (int i = 0; i < 4; i++) {
    pthread_create(&ts[i], nullptr, thread_func, &tids[i]);
  }
  sleep(1);

  const uint64_t tenant_id1 = 1001;
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.create_tenant_cgroup(tenant_id1));
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.add_thread_to_cgroup(tenant_id1, tids[0]));
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.add_thread_to_cgroup(tenant_id1, tids[1]));

  const uint64_t tenant_id2 = 1002;
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.create_tenant_cgroup(tenant_id2));
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.add_thread_to_cgroup(tenant_id2, tids[2]));
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.add_thread_to_cgroup(tenant_id2, tids[3]));

  ASSERT_EQ(OB_SUCCESS, cg_ctrl.remove_thread_from_cgroup(tenant_id2, tids[2]));
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.remove_thread_from_cgroup(tenant_id2, tids[3]));

  ASSERT_EQ(OB_SUCCESS, cg_ctrl.remove_tenant_cgroup(tenant_id1));

  for (int i = 0; i < 4; i++) {
    pthread_join(ts[i], nullptr);
  }
}

TEST(TestCgroupCtrl, SetGetValue)
{
  ObCgroupCtrl cg_ctrl;
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.init());
  ASSERT_TRUE(cg_ctrl.is_valid());

  int tids[4];
  pthread_t ts[4];
  for (int i = 0; i < 4; i++) {
    pthread_create(&ts[i], nullptr, thread_func, &tids[i]);
  }
  sleep(1);

  const uint64_t tenant_id1 = 1001;
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.create_tenant_cgroup(tenant_id1));
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.add_thread_to_cgroup(tenant_id1, tids[0]));
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.add_thread_to_cgroup(tenant_id1, tids[1]));

  const int32_t cpu_shares = 2048;
  int32_t cpu_shares_v = 0;
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.set_cpu_shares(tenant_id1, cpu_shares));
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.get_cpu_shares(tenant_id1, cpu_shares_v));
  ASSERT_EQ(cpu_shares, cpu_shares_v);

  const int32_t cpu_cfs_quota = 80000;
  int32_t cpu_cfs_quota_v = 0;
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.set_cpu_cfs_quota(tenant_id1, cpu_cfs_quota));
  ASSERT_EQ(OB_SUCCESS, cg_ctrl.get_cpu_cfs_quota(tenant_id1, cpu_cfs_quota_v));
  ASSERT_EQ(cpu_cfs_quota, cpu_cfs_quota_v);

  for (int i = 0; i < 4; i++) {
    pthread_join(ts[i], nullptr);
  }
}
int main(int argc, char *argv[])
{
  OB_LOGGER.set_file_name("test_cgroup_ctrl.log", true);
  OB_LOGGER.set_log_level("INFO");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

