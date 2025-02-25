/*
 * MIT License
Copyright (c) 2021 - current
Authors:  Animesh Trivedi
This code is part of the Storage System Course at VU Amsterdam
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

#include "device.h"

#include <libnvme.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "../common/nvmeprint.h"

// Examples lifted from,
// https://github.com/linux-nvme/libnvme/blob/667334ff8c53dbbefa51948bbe2e086624bf4d0d/test/cpp.cc
int count_and_show_all_nvme_devices() {
  nvme_root_t r;
  nvme_host_t h;
  nvme_subsystem_t s;
  nvme_ctrl_t c;
  nvme_path_t p;
  nvme_ns_t n;
  int count = 0;

  r = nvme_scan(NULL);
  if (!r) return -1;

  nvme_for_each_host(r, h) {
    nvme_for_each_subsystem(h, s) {
      std::cout << nvme_subsystem_get_name(s)
                << " - NQN=" << nvme_subsystem_get_nqn(s) << "\n";
      nvme_subsystem_for_each_ctrl(s, c) {
        std::cout << " `- " << nvme_ctrl_get_name(c) << " "
                  << nvme_ctrl_get_transport(c) << " "
                  << nvme_ctrl_get_address(c) << " " << nvme_ctrl_get_state(c)
                  << "\n";
        nvme_ctrl_for_each_ns(c, n) {
          std::cout << "   `- " << nvme_ns_get_name(n)
                    << "lba size:" << nvme_ns_get_lba_size(n)
                    << " lba max:" << nvme_ns_get_lba_count(n) << "\n";
        }
        nvme_ctrl_for_each_path(c, p) {
          std::cout << "   `- " << nvme_path_get_name(p) << " "
                    << nvme_path_get_ana_state(p) << "\n";
        }
        count++;
      }
    }
  }
  std::cout << "\n";
  nvme_free_tree(r);
  return count;
}

extern "C" {

int scan_and_identify_zns_devices(struct ss_nvme_ns *list) {
  int ret;
  int ns_counter = 0;
  nvme_root_t root;
  nvme_host_t h;
  nvme_subsystem_t subsystem;
  nvme_ctrl_t controller;
  nvme_ns_t nspace;
  nvme_id_ns ns{};

  root = nvme_scan(NULL /* for now the config file is NULL */);
  if (!root) {
    printf(
        "nvme_scan call failed with errno %d , null pointer returned in the "
        "scan call\n",
        -errno);
    return -1;
  }
  nvme_for_each_host(root, h) {
    nvme_for_each_subsystem(h, subsystem) {
      printf("root (%d) |- name: %s sysfs_dir %s subsysqn %s \n", ns_counter,
             nvme_subsystem_get_name(subsystem),
             nvme_subsystem_get_sysfs_dir(subsystem),
             nvme_subsystem_get_nqn(subsystem));
      nvme_subsystem_for_each_ctrl(subsystem, controller) {
        printf("\t|- controller : name %s (more to follow) \n ",
               nvme_ctrl_get_name(controller));
        nvme_ctrl_for_each_ns(controller, nspace) {
          printf(
              "\t\t|- namespace : name %s and command set identifier (csi) is "
              "%d (= 0 NVMe, 2 = ZNS), more to follow) \n ",
              nvme_ns_get_name(nspace), nvme_ns_get_csi(nspace));
          list[ns_counter].ctrl_name = strdup(nvme_ns_get_name(nspace));
          if (nvme_ns_get_csi(nspace) == NVME_CSI_ZNS) {
            list[ns_counter].supports_zns = true;
          } else {
            list[ns_counter].supports_zns = false;
          }
          // for convenience
          nvme_get_nsid(nvme_ns_get_fd(nspace), &list[ns_counter].nsid);
          ret = nvme_ns_identify(nspace, &ns);
          if (ret) {
            printf(
                "ERROR : failed to identify the namespace with %d and errno %d "
                "\n",
                ret, errno);
            return ret;
          }
          // nvme_show_id_ns(&ns);
          ns_counter++;
        }
      }
    }
  }
  nvme_free_tree(root);
  return 0;
}

int get_zns_zone_status(int fd, int nsid, char *&zone_reports) {
  // ZNS specific data structures as specified in the TP 4053
  struct nvme_zns_id_ns s_zns_nsid {};
  struct nvme_zns_id_ctrl s_zns_ctrlid {};
  struct nvme_zone_report zns_report {};
  struct nvme_zns_desc *desc = NULL;
  // standard NVMe structures
  struct nvme_id_ns s_nsid;
  int ret;
  uint64_t num_zones;
  // lets first get the NVMe ns identify structure (again), we need some
  // information from it to complement the information present in the ZNS ns
  // identify structure
  ret = nvme_identify_ns(fd, nsid, &s_nsid);
  if (ret != 0) {
    fprintf(stderr, "failed to identify NVMe namespace, ret %d \n", ret);
    return ret;
  }
  // see figure 8, section 3.1.1 in the ZNS specification
  ret = nvme_zns_identify_ns(fd, nsid, &s_zns_nsid);
  if (ret != 0) {
    fprintf(stderr, "failed to identify ZNS namespace, ret %d \n", ret);
    return -ret;
  }
  ss_nvme_show_zns_id_ns(&s_zns_nsid, &s_nsid);

  // 3.1.2, figure 10 in the ZNS specification
  ret = nvme_zns_identify_ctrl(fd, &s_zns_ctrlid);
  if (ret != 0) {
    fprintf(stderr, "failed to identify ZNS controller, ret %d \n", ret);
    return ret;
  }
  ss_nvme_show_zns_id_ctrl(&s_zns_ctrlid);

  // now we send the management related commands - see section 4.3 and 4.4 in TP
  // 4053 we are now trying to retrieve the number of zones with other
  // information present in the zone report the following function takes
  // arguments that are required to filled the command structure as shown in the
  // figures 33-36
  //   * SLBA goes into CDW 10 and 11, as shown in Figure 34
  //   * zras is Zone Receive Action Specific Features, see figure 36 for
  //   details
  //   * NVME_ZNS_ZRA_REPORT_ZONES and NVME_ZNS_ZRAS_REPORT_ALL are shown in
  //   Figure 36 CDW 13

  // Pay attention what is being passed in the zns_report pointer and size, I am
  // passing a structure _WITHOUT_ its entries[] field initialized because we do
  // not know how many zones does this namespace hence we first get the number
  // of zones, and then try again to get the full report
  ret = nvme_zns_mgmt_recv(fd, nsid, 0, NVME_ZNS_ZRA_REPORT_ZONES,
                           NVME_ZNS_ZRAS_REPORT_ALL, 0, sizeof(zns_report),
                           (void *)&zns_report);
  if (ret != 0) {
    fprintf(stderr, "failed to report zones, ret %d \n", ret);
    return ret;
  }
  // see figures 37-38-39 in section 4.4.1
  num_zones = le64_to_cpu(zns_report.nr_zones);
  printf("nr_zones:%" PRIu64 "\n", num_zones);
  // lets get more information about the zones - the total metadata size would
  // be see the figure 37 in the ZNS description so we allocated an structure
  // with a flat memory and point the zone_reports to it An alternate strategy
  // would have been just allocate a 4kB page and get some numbers of zone
  // reports whatever can fit in that in a loop.
  uint64_t total_size =
      sizeof(zns_report) + (num_zones * sizeof(struct nvme_zns_desc));
  zone_reports = (char *)calloc(1, total_size);
  ret = nvme_zns_mgmt_recv(fd, nsid, 0, NVME_ZNS_ZRA_REPORT_ZONES,
                           NVME_ZNS_ZRAS_REPORT_ALL, 1, total_size,
                           (void *)zone_reports);
  if (ret != 0) {
    fprintf(stderr, "failed to report zones, ret %d \n", ret);
    return ret;
  }
  desc = ((struct nvme_zone_report *)zone_reports)->entries;
  num_zones = le64_to_cpu(((struct nvme_zone_report *)zone_reports)->nr_zones);
  // otherwise we got all our reports, check again
  printf(
      "With the reports we have num_zones %lu (for which data transfer "
      "happened) \n",
      num_zones);
  for (uint64_t i = 0; i < num_zones; i++) {
    // see figure 39 for description of these fields
    printf("\t SLBA: 0x%-8" PRIx64 " WP: 0x%-8" PRIx64 " Cap: 0x%-8" PRIx64
           " State: %-12s Type: %-14s Attrs: 0x%-x\n",
           (uint64_t)le64_to_cpu(desc->zslba), (uint64_t)le64_to_cpu(desc->wp),
           (uint64_t)le64_to_cpu(desc->zcap),
           ss_zone_state_to_string(desc->zs >> 4),
           ss_zone_type_to_string(desc->zt), desc->za);

    printf("test: %lu", (uint64_t)le64_to_cpu(desc->zcap));
    desc++;
  }
  return ret;
}
}
