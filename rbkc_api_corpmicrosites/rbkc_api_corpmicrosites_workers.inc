<?php

function rbkc_api_corpmicrosites_allow_sites($item) {
  $allow_sites = $item['request_data']['allow_sites'];
  $uid = $item['request_data']['uid'];
  
  rbkc_api_microsites_grant_domains(user_load($uid), $allow_sites);
}
