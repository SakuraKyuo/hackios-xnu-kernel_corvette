/*
 * Included before every file of ath12k, after wifi/lx's prelude: what ath12k uses beyond ath11k,
 * of Linux's checking macros (which check nothing here) and of 802.11.
 */
#ifndef RCU_LOCKDEP_WARN
#define RCU_LOCKDEP_WARN(condition, message)    do { } while (0)
#endif

/*
 * 802.11be-2024, 9.4.2.323: how many bytes the Supported EHT-MCS And NSS Set field of an EHT
 * Capabilities element has, by the widths the HE and EHT capabilities say (3 for each of up to
 * 80, 160 and 320 MHz; a 20 MHz-only station's own field is 4, an access point's 3).
 */
static inline unsigned char
ieee80211_eht_mcs_nss_size(const struct ieee80211_he_cap_elem *he_cap,
    const struct ieee80211_eht_cap_elem_fixed *eht_cap, bool from_ap)
{
	unsigned char width = he_cap->phy_cap_info[0], n = 0;

	if (width & 0x02)                       /* B1: 40 MHz in 2.4 GHz */
		return (3);
	if (width & 0x04)                       /* B2: 40 and 80 MHz in 5 and 6 GHz */
		n += 3;
	if (width & 0x08)                       /* B3: 160 MHz in 5 and 6 GHz */
		n += 3;
	if (eht_cap->phy_cap_info[0] & 0x02)    /* EHT PHY B1: 320 MHz in 6 GHz */
		n += 3;
	return (n != 0 ? n : from_ap ? 3 : 4);
}

/*
 * For ath12k's monitor mode: what net80211's radiotap header lacks of EHT, from the radiotap
 * fields' definitions (https://www.radiotap.org/fields/EHT.html), and the names of the
 * enumerations ath12k declares variables of.
 */
enum ieee80211_radiotap_eht_data {
	IEEE80211_RADIOTAP_EHT_DATA0_DISREGARD_S		= 0x000c0000,
	IEEE80211_RADIOTAP_EHT_DATA0_DISREGARD_O		= 0x003c0000,
	IEEE80211_RADIOTAP_EHT_DATA0_CRC1_O			= 0x03c00000,
	IEEE80211_RADIOTAP_EHT_DATA1_RU_SIZE			= 0x0000001f,
	IEEE80211_RADIOTAP_EHT_DATA1_RU_INDEX			= 0x00001fe0,
};
enum lx12_radiotap_eht_known {
	IEEE80211_RADIOTAP_EHT_KNOWN_DISREGARD_O		= 0x00000100,
	IEEE80211_RADIOTAP_EHT_KNOWN_DISREGARD_S		= 0x00000200,
	IEEE80211_RADIOTAP_EHT_KNOWN_CRC1			= 0x00002000,
	IEEE80211_RADIOTAP_EHT_KNOWN_TAIL1			= 0x00004000,
	IEEE80211_RADIOTAP_EHT_KNOWN_RU_MRU_SIZE_OM		= 0x00400000,
	IEEE80211_RADIOTAP_EHT_KNOWN_RU_MRU_INDEX_OM		= 0x00800000,
};
enum lx12_radiotap_eht_user_info {
	IEEE80211_RADIOTAP_EHT_USER_INFO_SPATIAL_CONFIG_KNOWN_M	= 0x00000040,
	IEEE80211_RADIOTAP_EHT_USER_INFO_SPATIAL_CONFIG_M	= 0x3f000000,
};
enum ieee80211_radiotap_eht_usig_mu { LX12_RADIOTAP_EHT_USIG_MU };
enum ieee80211_radiotap_eht_usig_tb { LX12_RADIOTAP_EHT_USIG_TB };

/* Linux's name for an initialised list head, which LinuxKPI calls LINUX_LIST_HEAD_INIT */
#ifndef LIST_HEAD_INIT
#define LIST_HEAD_INIT(name)    LINUX_LIST_HEAD_INIT(name)
#endif

/* wifi/lx's stand-in for mac80211 queues nothing for sending (lx_80211.c): no queue is ever stopped */
static inline bool
ieee80211_queue_stopped(struct ieee80211_hw *hw, int queue)
{
	(void)hw;
	(void)queue;
	return (false);
}
