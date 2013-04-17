<?php
/**
 * @file
 * Stripe Payment controller class and helper code (classes and function).
 */

/**
 * Implements PaymentMethodController::payment_method_configuration_form_elements_callback.
 */
function stripe_payment_method_configuration_form_elements(array $form, array &$form_state) {
  return $form_state['payment_method']->controller
    ->payment_method_configuration_form_elements($form, $form_state);
}

/**
 * Implements validation for PaymentMethodController::payment_method_configuration_form_elements_callback.
 */
function stripe_payment_method_configuration_form_elements_validate(array $form, array &$form_state) {
  return $form_state['payment_method']->controller
    ->payment_method_configuration_form_elements_validate($form, $form_state);
}

/**
 * Implements PaymentMethodController::payment_configuration_form_elements_callback.
 */
function stripe_payment_configuration_form_elements(array $form, array &$form_state) {
  return $form_state['payment_method']->controller
    ->payment_configuration_form_elements($form, $form_state);
}

/**
 * Implements validation for PaymentMethodController::payment_configuration_form_elements_callback.
 */
function stripe_payment_configuration_form_elements_validate(array $form, array &$form_state) {
  return $form_state['payment_method']->controller
    ->payment_configuration_form_elements_validate($form, $form_state);
}

class StripePaymentValidationException extends PaymentValidationException {}

class StripePaymentMethodController extends PaymentMethodController {

  /**
   * The function name of the payment configuration form elements.
   */
  public $payment_configuration_form_elements_callback = 'stripe_payment_configuration_form_elements';

  /**
   * The function name of the payment method configuration form elements.
   */
  public $payment_method_configuration_form_elements_callback = 'stripe_payment_method_configuration_form_elements';

  /**
   * Default values for the controller_data property of a PaymentMethod that
   * uses this controller.
   */
  public $controller_data_defaults = array(
    'keys' => array(
      'mode' => 0,
      'secret' => '',
      'publishable' => '',
    ),
  );

  /**
   * Public constructor.
   */
  public function __construct() {
    $this->currencies = array_fill_keys(array('CAD', 'USD'), array('minimum' => 0.5));
    $this->title = t('Stripe Payment');
  }

  /**
   * Validate a payment against a payment method and this controller.
   *
   * Don't call directly. Use PaymentMethod::validate() instead.
   *
   * @see PaymentMethod::validate()
   *
   * @param Payment $payment
   *   The validated payment.
   * @param PaymentMethod $payment_method
   *   The payment method for the validated payment.
   * @param boolean $strict
   *   Whether to validate everything a payment method needs or to validate the
   *   most important things only. Useful when finding available payment
   *   methods, for instance, which does not require unimportant things to be a
   *   100% valid.
   *
   * @throws PaymentValidationException
   */
  public function validate(Payment $payment, PaymentMethod $payment_method, $strict) {
    // Update supported currencies using Stripe Account.
    $all_currencies = $this->currencies;
    if ($stripe_account = $this->retrieveAccount($payment_method)) {
      $payment_method->controller->currencies = array_intersect_key($this->currencies, $stripe_account->currencies_supported);
    }
    parent::validate($payment, $payment_method, $strict);
    // Restore supported currencies.
    $payment_method->controller->currencies = $all_currencies;
    if ($strict && !isset($payment->method_data['token'])
      && !isset($payment->method_data['card'])
      && !isset($payment->method_data['customer'])) {
      throw new StripePaymentValidationException("A Stripe payment must have one of 'token', 'card' or 'customer' payment method data.");
    }
  }

  /**
   * Execute a payment.
   *
   * Note that payments may be executed even if their owner is not logged into
   * the site. This means that if you need to do access control in your
   * execute() method, you cannot use global $user.
   *
   * @param Payment $payment
   *   The executed payment.
   *
   * @return boolean
   *   Whether the payment was successfully executed or not.
   */
  public function execute(Payment $payment) {
    $api_key = !empty($payment->method->controller_data['keys']['mode']) ? $payment->method->controller_data['keys']['secret'] : stripe_get_key('secret');
    try {
      if (empty($payment->method_data['charge_id'])) {
        // Create the Stripe Charge for the payment.
        $data = array(
          'amount' => $payment->totalAmount(TRUE),
          'currency' => $payment->currency_code,
          'description' => t($payment->description, $payment->description_arguments),
        );
        if (isset($payment->method_data['token'])) {
          $data['card'] = $payment->method_data['token'];
        }
        elseif (isset($payment->method_data['card'])) {
          $data['card'] = $payment->method_data['card'];
        }
        elseif (isset($payment->method_data['customer'])) {
          $data['customer'] = $payment->method_data['customer'];
        }
        if (isset($payment->method_data['capture'])) {
          $data['capture'] = $payment->method_data['capture'];
        }
        $charge = Stripe_Charge::create($data, $api_key);
        watchdog(
          'stripe_payment',
          'Stripe Charge [id=@charge_id] for Payment [pid=@pid] successful.',
          array(
            '@charge_id' => $payment->method_data['charge_id'],
            '@pid' => $payment->pid,
          ),
          WATCHDOG_DEBUG, url("payment/{$payment->pid}")
        );
        $payment->setStatus($this->statusFromCharge($charge));
        $payment->method_data['charge_id'] = $charge->id;
      }
      else {
        // The payment is already linked to a Stripe Charge. This should not
        // happen. Let's update the payment status and pretend everything went
        // fine.
        watchdog('stripe_payment', 'Executing a payment already linked to a Stripe Charge [id=@charge_id].', array(
          '@charge_id' => $payment->method_data['charge_id'],
        ), WATCHDOG_WARNING, url("payment/{$payment->pid}"));
        $charge = Stripe_Charge::retrieve($payment->method_data['charge_id'], $api_key);
        $payment->setStatus($this->statusFromCharge($charge));
      }
      // If we get to this LoC, then the payment execution has succeeded.
      return TRUE;
    }
    catch (Stripe_CardError $e) {
      $payment->setStatus($this->statusFronCardErrorCode($e->getCode()));
      // Display human-readable message.
      if ($e->getMessage()) {
        drupal_set_message($e->getMessage(), 'error');
      }
      watchdog(
        'stripe_payment',
        'Stripe Card Error: @message.',
        array('@message' => $e->getMessage()),
        WATCHDOG_DEBUG
      );
    }
    catch (Stripe_InvalidRequestError $e) {
      $payment->setStatus(new PaymentStatusItem(STRIPE_PAYMENT_STATUS_INVALID_REQUEST));
      watchdog(
        'stripe_payment',
        'Invalid Stripe Request: @message.<br/>!json_body',
        array(
          '@message' => $e->getMessage(),
          '!json_body' => '<?php ' . highlight_string(var_export($e->getJsonBody(), TRUE), TRUE),
        ),
        WATCHDOG_ERROR
      );
    }
    catch (Stripe_AuthenticationError $e) {
      $payment->setStatus(new PaymentStatusItem(STRIPE_PAYMENT_STATUS_AUTHENTICATION_ERROR));
      if (payment_method_access('update', $payment->method)) {
        drupal_set_message(
          t("Stripe Authentication Error: @message. Please review <a href='@url'>configuration</a>.",
            array(
              '@message' => $e->getMessage(),
              '@url' => url("admin/config/services/payment/method/{$payment->method->pmid}"),
            )
          ), 'error');
      }
      watchdog(
        'stripe_payment',
        'Stripe Authentication Error: @message.',
        array('@message' => $e->getMessage()),
        WATCHDOG_ERROR,
        url("admin/config/services/payment/method/{$payment->method->pmid}")
      );
    }
    catch (Stripe_ApiError $e) {
      $payment->setStatus(new PaymentStatusItem(STRIPE_PAYMENT_STATUS_API_ERROR));
      watchdog(
        'stripe_payment',
        'Stripe API Error: @message.<br/>!json_body',
        array(
          '@message' => $e->getMessage(),
          '!json_body' => '<?php ' . highlight_string(var_export($e->getJsonBody(), TRUE), TRUE),
        ),
        WATCHDOG_ERROR
      );
    }
    catch (Exception $e) {
      $payment->setStatus(new PaymentStatusItem(STRIPE_PAYMENT_STATUS_UNKNOWN_ERROR));
      watchdog(
        'stripe_payment',
        'Stripe unknown Error: @message.<br/>!json_body',
        array(
          '@message' => $e->getMessage(),
          '!json_body' => '<?php ' . highlight_string(var_export($e->getJsonBody(), TRUE), TRUE),
        ),
        WATCHDOG_ERROR
      );
    }
    // The only way to reach this LoC is to exit the try block with an
    // exception. Thus, the payment execution has failed.
    return FALSE;
  }

  /**
   * Retrieve the Stripe Account object for a given Payment Method.
   *
   * This method handles Stripe errors and logs them to Drupal's watchdog.
   *
   * @param PaymentMethod $payment_method
   *   A Payment Method.
   *
   * @return Stripe_Account
   *   The Stripe Account object for the given payment method. Or NULL if the
   *   account could not be retrieved (errors are logged to Drupal's watchdog).
   */
  public static function retrieveAccount(PaymentMethod $payment_method) {
    if ($payment_method->controller->name === 'StripePaymentMethodController') {
      $accounts[] = &drupal_static(__METHOD__, array());
      if (!isset($accounts[$payment_method->pmid])) {
        $cid = "{$payment_method->pmid}:account";
        $cache = cache_get($cid, 'stripe_payment');
        if ($cache && !empty($cache->data)) {
          $accounts[$payment_method->pmid] = $cache->data;
        }
        else {
          $api_key = !empty($payment_method->controller_data['keys']['mode']) ? $payment_method->controller_data['keys']['secret'] : stripe_get_key('secret');
          try {
            $stripe_account = Stripe_Account::retrieve($api_key);
            cache_set($cid, $stripe_account, 'strip_payment', CACHE_TEMPORARY);
            $accounts[$payment_method->pmid] = $stripe_account;
          }
          catch (Stripe_Error $e) {
            if (payment_method_access('update', $payment_method)) {
              drupal_set_message(
                t("Unable to retrieve Stripe Account for <a href='@url'>Payment Method</a>: @message.",
                  array(
                    '@message' => $e->getMessage(),
                    '@url' => url("admin/config/services/payment/method/{$payment_method->pmid}"),
                  )
                ), 'error');
            }
            watchdog(
              'stripe_payment',
              "Unable to retrieve Stripe Account for <a href='@url'>Payment Method</a>: @message.",
              array(
                '@message' => $e->getMessage(),
                '@url' => url("admin/config/services/payment/method/{$payment_method->pmid}"),
              ),
              WATCHDOG_ERROR,
              url("admin/config/services/payment/method/{$payment_method->pmid}")
            );
            $accounts[$payment_method->pmid] = NULL;
          }
        }
      }
      return $accounts[$payment_method->pmid];
    }
    else {
      return NULL;
    }
  }

  /**
   * Retrieve the Stripe Charge object for a given Payment.
   *
   * This method handles Stripe errors and logs them to Drupal's watchdog.
   *
   * @param Payment $payment
   *   A Payment object.
   *
   * @return Stripe_Charge
   *   The Stripe Charge object for the given Payment. Or NULL if no Stripe
   *   Charge could be retrieved (errors are logged to Drupal's watchdog).
   */
  public static function retrieveCharge(Payment $payment) {
    if ($payment->method->controller->name === 'StripePaymentMethodController' && isset($payment->method_data['charge_id'])) {
      $cid = "{$payment->method->pmid}:charge:{$payment->method_data['charge_id']}";
      $cache = cache_get($cid, 'stripe_payment');
      if ($cache && !empty($cache->data)) {
        return $cache->data;
      }
      else {
        $api_key = !empty($payment->method->controller_data['keys']['mode']) ? $payment->method->controller_data['keys']['secret'] : stripe_get_key('secret');
        try {
          $charge = Stripe_Charge::retrieve($payment->method_data['charge_id'], $api_key);
          cache_set($cid, $charge, 'strip_payment', CACHE_TEMPORARY);
          return $charge;
        }
        catch (Stripe_Error $e) {
          if (payment_access('view', $payment)) {
            drupal_set_message(
              t('Unable to retrieve Stripe Charge for <a href="@url">Payment</a>: @message.',
                array(
                  '@url' => url("payment/{$payment->pid}"),
                  '@message' => $e->getMessage(),
                )
              ), 'error');
          }
          watchdog(
            'stripe_payment',
            'Unable to retrieve Stripe Charge for <a href="@url">Payment</a>: @message.',
            array(
              '@url' => url("payment/{$payment->pid}"),
              '@message' => $e->getMessage(),
            ),
            WATCHDOG_ERROR,
            url("payment/{$payment->pid}")
          );
        }
      }
    }
    return NULL;
  }

  /**
   * Map a card error code to a Payment status.
   *
   * @paran string $code
   *   A Stripe card error code.
   *
   * @return PaymentStatusItem
   *   The Payment status matching the error code.
   */
  protected function statusFronCardErrorCode($code) {
    if (epmty($code)) {
      return new PaymentStatusItem(PAYMENT_STATUS_FAILED);
    }
    $status = 'STRIPE_PAYMENT_STATUS_' . drupal_strtoupper($code);
    if (defined($status)) {
      return new PaymentStatusItem($status);
    }
    else {
      return new PaymentStatusItem(PAYMENT_STATUS_FAILED);
    }
  }

  /**
   * Map a Strip Charge object to a Payment status.
   *
   * @param Stripe_Charge $charge
   *   A stripe charge object.
   *
   * @return PaymentStatusItem
   *   The Payment status matching the charge object.
   */
  protected function statusFromCharge(Stripe_Charge $charge) {
    if ($charge->paid) {
      return new PaymentStatusItem(STRIPE_PAYMENT_STATUS_PAID);
    }
    elseif (!$charge->captured) {
      return new PaymentStatusItem(STRIPE_PAYMENT_STATUS_UNCAPTURED);
    }
    else {
      return new PaymentStatusItem(PAYMENT_STATUS_UNKNOWN);
    }
  }

  /**
   * Builder for the method configuration form elements.
   *
   * @param array $element
   *   The parent element.
   * @param array $form_state
   *   The form states.
   *
   * TODO: Allow admin to configure a payment method WITHOUT Stripe.js.
   *       The card information for the method's Payment will then be be stored
   *       in the Payment::method_data. Provide clear and visible warning with
   *       information regarding the security risk and PCI compliance.
   *
   * @return array
   *   The forms elements to configure a payment method.
   */
  public function payment_method_configuration_form_elements(array $element, array &$form_state) {
    $controller_data = $form_state['payment_method']->controller_data + $this->controller_data_defaults;
    $elements = array();
    $elements['keys'] = array(
      '#type' => 'fieldset',
      '#title' => t('Authentication'),
      '#tree' => TRUE,
      'mode' => array(
        '#type' => 'select',
        '#options' => array(
          0 => t('Use site-wide keys'),
          1 => t('Use specific keys'),
        ),
        '#attributes' => array('class' => array('keys-mode')),
        '#default_value' => $controller_data['keys']['mode'],
      ),
      'site-wide' => array(
        '#type' => 'item',
        '#markup' => t('Using site-wide %status keys as configured on the <a href="@url">Stripe settings page</a>.', array(
            '%status' => variable_get('stripe_key_status', 'test'),
            '@url' => url('admin/config/stripe/settings'),
          )),
        '#states' => array(
          'visible' => array(
            ':input.keys-mode' => array('value' => 0),
          ),
        ),
      ),
      'secret' => array(
        '#type' => 'textfield',
        '#title' => t('Secret Key'),
        '#states' => array(
          'visible' => array(
            ':input.keys-mode' => array('value' => 1),
          ),
          'required' => array(
            ':input.keys-mode' => array('value' => 1),
          ),
        ),
        '#default_value' => $controller_data['keys']['secret'],
      ),
      'publishable' => array(
        '#type' => 'textfield',
        '#title' => t('Publishable Key'),
        '#states' => array(
          'visible' => array(
            ':input.keys-mode' => array('value' => 1),
          ),
          'required' => array(
            ':input.keys-mode' => array('value' => 1),
          ),
        ),
        '#default_value' => $controller_data['keys']['publishable'],
      ),
    );
    return $elements;
  }

  /**
   * Validate callback for the method configuration form elements.
   *
   * @param array $element
   *   The parent element.
   * @param array $form_state
   *   The form states.
   */
  public function payment_method_configuration_form_elements_validate(array $element, array &$form_state) {
    $values = drupal_array_get_nested_value($form_state['values'], $element['#parents']);
    if ($values['keys']['mode']) {
      foreach (array('secret' => 'sk', 'publishable' => 'pk') as $type => $prefix) {
        if (empty($values['keys'][$type])) {
          form_error($element['keys'][$type], t('!name field is required.', array('!name' => $element['keys'][$type]['#title'])));
        }
        elseif (strpos($values['keys'][$type], $prefix) !== 0) {
          form_error($element['keys'][$type], t('!name should start with %prefix.', array(
            '!name' => $element['keys'][$type]['#title'],
            '%prefix' => $prefix,
          )));
        }
      }
    }
    $form_state['payment_method']->controller_data['keys'] = $values['keys'];
  }

  /**
   * Builder callback for the payment configuration form elements.
   *
   * @param array $element
   *   The parent element.
   * @param array $form_state
   *   The form states.
   *
   * TODO: Support form WITHOUT Stripe.js.
   *      (see TODO for payment_method_configuration_form_elements)
   *
   * @return array
   *   The forms elements to configure a payment.
   */
  public function payment_configuration_form_elements(array $element, array &$form_state) {
    return array();
  }

  /**
   * Validate callback for the payment configuration form elements.
   *
   * @param array $element
   *   The parent element.
   * @param array $form_state
   *   The form states.
   */
  public function payment_configuration_form_elements_validate(array $element, array &$form_state) {
    $values = drupal_array_get_nested_value($form_state['values'], $element['#parents']);
    $payment = $form_state['payment'];
    $payment->method_data['token'] = $values['stripe_token'];
    return array();
  }
}