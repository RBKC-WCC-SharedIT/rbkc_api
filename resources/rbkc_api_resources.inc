<?php

define('RBKC_API_DEFAULT_REQUESTER_UID', 1);

class APIUserNotFoundException extends Exception {}

function _rbkc_api_access_retrieve() {
	return TRUE;
}

function _rbkc_api_needs_review_retrieve($sitename = '') {
  $callback_name = '_rbkc_api_workbench_get_needs_review_count';
  drupal_alter('needs_review_callback', $callback_name); // Generate hook_needs_review_callback_alter
  return $callback_name($sitename);
}

/* WORKBENCH RESOURCE */

function _rbkc_api_workbench_get_needs_review_count() {
  $query = db_select('workbench_moderation_node_history', 'wmnh')
    ->fields('wmnh');
  $query->condition('wmnh.state', "needs_review"); // only want 'needs_review' status
  $query->condition('wmnh.current', 1);  // where needs_review is the CURRENT status (table stores history)
  $results = $query->execute();
   
  return $results->rowCount(); // return the number of rows found
}


/* USER RESOURCE */

function _rbkc_api_user_create($user_data) {
	global $user;
	$q_item = array(
		'worker_include_file' => array(
			'type' => 'inc',
			'module' => 'rbkc_api',
			'name' => 'resources/rbkc_api_resources',
		),
		'worker_callback' => 'rbkc_api_user_create_queue_worker',
    'requester_uid' => isset($user_data['requester_uid'])? $user_data['requester_uid'] : RBKC_API_DEFAULT_REQUESTER_UID,
		'request_data' => $user_data,
	);
	drupal_alter('rbkc_api_queue_item', $q_item);
	rbkc_api_queue_add($q_item);
	return 'Queued user creation for: '.$user_data['username'].' <'.$user_data['email'].'>';
}

function _rbkc_api_user_update($username, $user_data) {
  $q_item = array(
    'worker_include_file' => array(
      'type' => 'inc',
      'module' => 'rbkc_api',
      'name' => 'resources/rbkc_api_resources',
    ),
    'worker_callback' => 'rbkc_api_user_update_queue_worker',
    'requester_uid' => isset($user_data['requester_uid'])? $user_data['requester_uid'] : RBKC_API_DEFAULT_REQUESTER_UID,
    'request_data' => $user_data,
    'target_username' => $username,
  );
  drupal_alter('rbkc_api_queue_item', $q_item);
  rbkc_api_queue_add($q_item);
  return 'Queued user update for: '.$user_data['username'].' <'.$user_data['email'].'>';
}

function _rbkc_api_user_revoke($user_data) {
  $q_item = array(
    'worker_include_file' => array(
      'type' => 'inc',
      'module' => 'rbkc_api',
      'name' => 'resources/rbkc_api_resources',
    ),
    'worker_callback' => 'rbkc_api_user_revoke_queue_worker',
    'requester_uid' => isset($user_data['requester_uid'])? $user_data['requester_uid'] : RBKC_API_DEFAULT_REQUESTER_UID,
    'request_data' => $user_data,
  );
  drupal_alter('rbkc_api_queue_item', $q_item);
  rbkc_api_queue_add($q_item);
  return 'Queued user revoke for: '.$user_data['username'].' <'.$user_data['email'].'>';
}

/********************************/

/**
 * User create callback. This is run when an incoming user-create request
 * is being picked up from the queue and processed. If user exists, then
 * unblock the user account, instead of creating it.
 * @param array $item The queue item being processed.
 * @return null
 */
function rbkc_api_user_create_queue_worker($item) {
  
  module_invoke_all('rbkc_api_user_pre_create', $item);
  
  $target_user = user_load_by_name($item['request_data']['username']);
  
  if ($target_user) {
    if ($target_user->status == 0) {
      rbkc_api_user_unblock($target_user, $item['request_data']);
    }
    $usermode = RBKC_API_EXISTING_USER;
  } else {
    $target_user = _rbkc_api_user_create_helper($item);
    $usermode = RBKC_API_NEW_USER;
  }
  
  module_invoke_all('rbkc_api_user_post_create', $target_user, $item, $usermode);
  
  return true;
  
}

/**
 * Helper function to create a new user
 * @param array $queue_item The queue item for the user-create request
 * @return \stdClass
 */
function _rbkc_api_user_create_helper($queue_item) {
  $fields1 = _rbkc_api_create_usersave_data($queue_item['request_data']);
  
  $fields2 = array(
    'pass' => user_password(8),
    'status' => 1,
    'init' => $queue_item['request_data']['mail'],
  );

  $fields = $fields1 + $fields2;
  
  $account = user_save(null, $fields);
  
  return $account;
}

/**
 * Creates an array of user profile data into data for user_save()
 * @param $source_data
 * @return array
 */
function _rbkc_api_create_usersave_data($source_data) {
  $userdata = array();
  foreach ($source_data as $field => $value) {
    switch ($field) {
      case 'fullname':
        $userdata['field_full_name'][LANGUAGE_NONE][0]['value'] = $value;
        break;
      case 'username':
        $userdata['name'] = $value;
        break;
      case 'email':
        $userdata['mail'] = $value;
        break;
      case 'roles':
        $userdata['roles'] = _rbkc_api_get_roles($value);
        break;
      case 'status':
        $userdata['status'] = $value;
        break;
    }
  }
  return $userdata;
}

/**
 * Unblock a user
 * @param object $user
 * @param array $update_data
 */
function rbkc_api_user_unblock($user, $update_data = null) {
  $user_data = _rbkc_api_create_usersave_data($update_data);
  $user_data['status'] = 1;
  user_save($user, $user_data);
  watchdog('rbkc_api', 'Unblocked username '.$user->name);
}

/**
 * User update callback. This is run when an incoming user-update request
 * is being picked up from the queue and processed.
 * @param array $item The queue item being processed.
 * @return null
 * @throws \APIUserNotFoundException
 */
function rbkc_api_user_update_queue_worker($item) {
  if (!isset($item['request_data']['target_username'])) {
    throw new APIUserNotFoundException("Target username was not provided in REST user update request");
  }
  
  $target_username = $item['request_data']['target_username'];
  $target_user_obj = user_load_by_name($target_username);
  
  if (empty($target_user_obj)) {
    throw new APIUserNotFoundException('Received API request update user '.$target_username.' but username is not found.');
  }
  
  $save_data = _rbkc_api_create_usersave_data($item['request_data']);
  
  if ($save_data) {
    user_save($target_user_obj, $save_data);
  }
}

function rbkc_api_user_revoke_queue_worker($item) {
  watchdog('rbkc_api_user_revoke_queue_worker', '$item: '.print_r($item, true));
  if (!isset($item['request_data']['target_username'])) {
    watchdog('rbkc_api', 'User revoke request ignored. Username not provided. Queue item dump: '.print_r($item, true));
    return true;
  }
  
  $username = $item['request_data']['target_username'];
  $target_user = user_load_by_name($username);
  
  if ($target_user === false) {
    watchdog('rbkc_api', 'User revoke request ignored. Username "'.$username.'" not found in database. Queue item dump: '.print_r($item, true));
    return true;
  }
  
  // Blocked status is 0
  if ($target_user->status != 0) {
    user_save($target_user, array('status' => 0));
    watchdog('rbkc_api', 'Revoked access for username "'.$username.'". Queue item dump: '.print_r($item, true));
  }
  
  return true;
}

/**
 * Translates an array of rol names into array of rid => role_name
 * @param  array $roles 1-dimension array of role names
 * @return array
 */
function _rbkc_api_get_roles($roles) {
	$rid_roles = array(DRUPAL_AUTHENTICATED_RID => 'authenticated user');
	$sitewide_roles = user_roles(true); // get all roles on the site
	foreach ($sitewide_roles as $swr_rid => $swr_name) {
		if (in_array($swr_name, $roles)) {
			$rid_roles[$swr_rid] = $swr_name;
		}
	}
	return $rid_roles;
}