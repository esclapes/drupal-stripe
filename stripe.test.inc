<?php

/**
 * @file
 * Stripe test form building callback and handlers.
 */

function stripe_admin_test() {
  $form = stripe_form_default_items();
  $form['card_number']['#description'] = t('Use 4242424242424242 for testing');
  $form['card_cvc']['#description'] = t('Use 123 for testing');
  $form['card_expiry_month']['#default_value'] = '10';
  $form['card_expiry_year']['#default_value'] = '2020';
  $form['amount'] = array(
    '#type' => 'textfield',
    '#title' => t('Amount:'),
    '#description' => '<em>cents</em>',
    '#default_value' => 99,
    '#size' => 6,
    '#attributes' => array(
      'class' => array('amount'),
    ),
    '#weight' => 2,
  );
  $form['card_submit']['#weight'] = 3; 
  $form['card_submit'] = array(
      '#type' => 'submit',
      '#value' => t('Submit Payment'),
      '#attributes' => array(
      'class' => array('submit-button'),
    ),
  );
  return $form;
}


function stripe_admin_test_submit($form, &$form_state) {
  require_once("sites/all/libraries/stripe/lib/Stripe.php");
  
  $status = variable_get('stripe_key_status', 'test');
  $secret_key_name = 'stripe_' . $status . '_secret';
  $secret_key = variable_get($secret_key_name, '');
  
  Stripe::setApiKey($secret_key);
  
  $charge = Stripe_Charge::create(array(
    "amount" => $form_state['values']['amount'], // amount in cents, again
    "currency" => "usd",
    "card" => $form_state['values']['stripe_token'],
    "description" => "Test Charge from " . variable_get('site_name', 'My Drupal Site'))
  );
  //TODO Check on error handling here;
  drupal_set_message("Success! Card was successfully charged for the amount of " . check_plain($form_state['values']['amount']));
}