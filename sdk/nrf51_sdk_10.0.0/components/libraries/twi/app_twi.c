/* Copyright (c) 2015 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */

#include <stdbool.h>
#include "app_twi.h"
#include "nrf_assert.h"
#include "app_util_platform.h"


// Increase specified queue index and when it goes outside the queue move it
// on the beginning of the queue.
#define INCREASE_IDX(idx, p_queue)  \
    do { \
        ++idx; \
        p_queue->idx = (idx > p_queue->size) ? 0 : idx; \
    } while (0)


static bool queue_put(app_twi_queue_t *             p_queue,
                      app_twi_transaction_t const * p_transaction)
{
    // [use a local variable to avoid using two volatile variables in one
    //  expression]
    uint8_t write_idx = p_queue->write_idx;

    // If the queue is already full, we cannot put any more elements into it.
    if ((write_idx == p_queue->size && p_queue->read_idx == 0) ||
        write_idx == p_queue->read_idx-1)
    {
        return false;
    }

    // Write the new element on the position specified by the write index.
    p_queue->p_buffer[write_idx] = p_transaction;
    // Increase the write index and when it goes outside the queue move it
    // on the beginning.
    INCREASE_IDX(write_idx, p_queue);

    return true;
}


static app_twi_transaction_t const * queue_get(app_twi_queue_t * p_queue)
{
    // [use a local variable to avoid using two volatile variables in one
    //  expression]
    uint8_t read_idx = p_queue->read_idx;

    // If the queue is empty, we cannot return any more elements from it.
    if (read_idx == p_queue->write_idx)
    {
        return NULL;
    }

    // Read the element from the position specified by the read index.
    app_twi_transaction_t const * p_transaction = p_queue->p_buffer[read_idx];
    // Increase the read index and when it goes outside the queue move it
    // on the beginning.
    INCREASE_IDX(read_idx, p_queue);

    return p_transaction;
}


static ret_code_t start_transfer(app_twi_t const * p_app_twi)
{
    ASSERT(p_app_twi != NULL);

    // [use a local variable to avoid using two volatile variables in one
    //  expression]
    uint8_t current_transfer_idx = p_app_twi->current_transfer_idx;
    app_twi_transfer_t const * p_transfer =
        &p_app_twi->p_current_transaction->p_transfers[current_transfer_idx];
    uint8_t address = APP_TWI_OP_ADDRESS(p_transfer->operation);

    if (APP_TWI_IS_READ_OP(p_transfer->operation))
    {
        return nrf_drv_twi_rx(&p_app_twi->twi, address,
            p_transfer->p_data, p_transfer->length,
            (p_transfer->flags & APP_TWI_NO_STOP));
    }
    else
    {
        return nrf_drv_twi_tx(&p_app_twi->twi, address,
            p_transfer->p_data, p_transfer->length,
            (p_transfer->flags & APP_TWI_NO_STOP));
    }
}


static void signal_end_of_transaction(app_twi_t const * p_app_twi,
                                      ret_code_t        result)
{
    ASSERT(p_app_twi != NULL);

    if (p_app_twi->p_current_transaction->callback)
    {
        // [use a local variable to avoid using two volatile variables in one
        //  expression]
        void * p_user_data = p_app_twi->p_current_transaction->p_user_data;
        p_app_twi->p_current_transaction->callback(result, p_user_data);
    }
}


// This function starts pending transaction if there is no current one or
// when 'switch_transaction' parameter is set to true. It is important to
// switch to new transaction without setting 'p_app_twi->p_current_transaction'
// to NULL in between, since this pointer is used to check idle status - see
// 'app_twi_is_idle()'.
static void start_pending_transaction(app_twi_t * p_app_twi,
                                      bool        switch_transaction)
{
    ASSERT(p_app_twi != NULL);

    for (;;)
    {
        bool start_transaction = false;

        CRITICAL_REGION_ENTER();
        if (switch_transaction || app_twi_is_idle(p_app_twi))
        {
            p_app_twi->p_current_transaction = queue_get(&p_app_twi->queue);
            if (p_app_twi->p_current_transaction != NULL)
            {
                start_transaction = true;
            }
        }
        CRITICAL_REGION_EXIT();

        if (!start_transaction)
        {
            return;
        }
        else
        {
            ret_code_t result;

            // Try to start first transfer for this new transaction.
            p_app_twi->current_transfer_idx = 0;
            result = start_transfer(p_app_twi);

            // If it started successfully there is nothing more to do here now.
            if (result == NRF_SUCCESS)
            {
                return;
            }

            // Transfer failed to start - notify user that this transaction
            // cannot be started and try with next one (in next iteration of
            // the loop).
            signal_end_of_transaction(p_app_twi, result);

            switch_transaction = true;
        }
    }
}


static void twi_event_handler(nrf_drv_twi_evt_t const * p_event,
                              void *                    p_context)
{
    ASSERT(p_event != NULL);

    app_twi_t * p_app_twi = (app_twi_t *)p_context;
    ret_code_t result;

    // This callback should be called only during transaction.
    ASSERT(p_app_twi->p_current_transaction != NULL);

    if (p_event->type != NRF_DRV_TWI_ERROR)
    {
        result = NRF_SUCCESS;

        // Transfer finished successfully. If there is another one to be
        // performed in the current transaction, start it now.
        // [use a local variable to avoid using two volatile variables in one
        //  expression]
        uint8_t current_transfer_idx = p_app_twi->current_transfer_idx;
        ++current_transfer_idx;
        if (current_transfer_idx <
                p_app_twi->p_current_transaction->number_of_transfers)
        {
            p_app_twi->current_transfer_idx = current_transfer_idx;

            result = start_transfer(p_app_twi);

            if (result == NRF_SUCCESS)
            {
                // The current transaction goes on and we've successfully
                // started its next transfer -> there is nothing more to do.
                return;
            }

            // [if the next transfer could not be started due to some error
            //  we finish the transaction with this error code as the result]
        }
    }
    else
    {
        result = NRF_ERROR_INTERNAL;
    }

    // The current transaction has been completed or interrupted by some error.
    // Notify the user and start next one (if there is any).
    signal_end_of_transaction(p_app_twi, result);
    // [we switch transactions here ('p_app_twi->p_current_transaction' is set
    //  to NULL only if there is nothing more to do) in order to not generate
    //  spurious idle status (even for a moment)]
    start_pending_transaction(p_app_twi, true);
}


ret_code_t app_twi_init(app_twi_t *                     p_app_twi,
                        nrf_drv_twi_config_t const *    p_twi_config,
                        uint8_t                         queue_size,
                        app_twi_transaction_t const * * p_queue_buffer)
{
    ASSERT(p_app_twi != NULL);
    ASSERT(queue_size != 0);
    ASSERT(p_queue_buffer != NULL);

    ret_code_t err_code;

    err_code = nrf_drv_twi_init(&p_app_twi->twi,
                                p_twi_config,
                                twi_event_handler,
                                p_app_twi);
    if (err_code != NRF_SUCCESS)
    {
        return err_code;
    }
    nrf_drv_twi_enable(&p_app_twi->twi);

    p_app_twi->queue.p_buffer  = p_queue_buffer;
    p_app_twi->queue.size      = queue_size;
    p_app_twi->queue.read_idx  = 0;
    p_app_twi->queue.write_idx = 0;

    p_app_twi->internal_transaction_in_progress = false;
    p_app_twi->p_current_transaction            = NULL;

    return NRF_SUCCESS;
}


void app_twi_uninit(app_twi_t * p_app_twi)
{
    ASSERT(p_app_twi != NULL);

    nrf_drv_twi_uninit(&(p_app_twi->twi));

    p_app_twi->p_current_transaction = NULL;
}


ret_code_t app_twi_schedule(app_twi_t *                   p_app_twi,
                            app_twi_transaction_t const * p_transaction)
{
    ASSERT(p_app_twi != NULL);
    ASSERT(p_transaction != NULL);
    ASSERT(p_transaction->p_transfers != NULL);
    ASSERT(p_transaction->number_of_transfers != 0);

    ret_code_t result = NRF_SUCCESS;

    CRITICAL_REGION_ENTER();
    if (!queue_put(&p_app_twi->queue, p_transaction))
    {
        result = NRF_ERROR_BUSY;
    }
    CRITICAL_REGION_EXIT();

    if (result == NRF_SUCCESS)
    {
        // New transaction has been successfully added to queue,
        // so if we are currently idle it's time to start the job.
        start_pending_transaction(p_app_twi, false);
    }

    return result;
}


static void internal_transaction_cb(ret_code_t result, void * p_user_data)
{
    app_twi_t * p_app_twi = (app_twi_t *)p_user_data;

    p_app_twi->internal_transaction_result      = result;
    p_app_twi->internal_transaction_in_progress = false;
}


ret_code_t app_twi_perform(app_twi_t *                p_app_twi,
                           app_twi_transfer_t const * p_transfers,
                           uint8_t                    number_of_transfers,
                           void (* user_function)(void))
{
    ASSERT(p_app_twi != NULL);
    ASSERT(p_transfers != NULL);
    ASSERT(number_of_transfers != 0);

    bool busy = false;

    CRITICAL_REGION_ENTER();
    if (p_app_twi->internal_transaction_in_progress)
    {
        busy = true;
    }
    else
    {
        p_app_twi->internal_transaction_in_progress = true;
    }
    CRITICAL_REGION_EXIT();

    if (busy)
    {
        return NRF_ERROR_BUSY;
    }
    else
    {
        app_twi_transaction_t internal_transaction =
        {
            .callback            = internal_transaction_cb,
            .p_user_data         = p_app_twi,
            .p_transfers         = p_transfers,
            .number_of_transfers = number_of_transfers,
        };
        ret_code_t result = app_twi_schedule(p_app_twi, &internal_transaction);
        if (result != NRF_SUCCESS)
        {
            return result;
        }

        while (p_app_twi->internal_transaction_in_progress)
        {
            if (user_function)
            {
                user_function();
            }
        }

        return p_app_twi->internal_transaction_result;
    }
}
