/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>
#include "gfileenumerator.h"
#include "gioscheduler.h"
#include "gasynchelper.h"
#include "gsimpleasyncresult.h"
#include "glibintl.h"

/**
 * SECTION:gfileenumerator
 * @short_description: Enumerated Files Routines.
 * 
 * 
 **/ 

G_DEFINE_TYPE (GFileEnumerator, g_file_enumerator, G_TYPE_OBJECT);

struct _GFileEnumeratorPrivate {
  /* TODO: Should be public for subclasses? */
  guint closed : 1;
  guint pending : 1;
  GAsyncReadyCallback outstanding_callback;
  GError *outstanding_error;
};

static void     g_file_enumerator_real_next_files_async  (GFileEnumerator      *enumerator,
							  int                   num_files,
							  int                   io_priority,
							  GCancellable         *cancellable,
							  GAsyncReadyCallback   callback,
							  gpointer              user_data);
static GList *  g_file_enumerator_real_next_files_finish (GFileEnumerator      *enumerator,
							  GAsyncResult         *res,
							  GError              **error);
static void     g_file_enumerator_real_close_async       (GFileEnumerator      *enumerator,
							  int                   io_priority,
							  GCancellable         *cancellable,
							  GAsyncReadyCallback   callback,
							  gpointer              user_data);
static gboolean g_file_enumerator_real_close_finish      (GFileEnumerator      *enumerator,
							  GAsyncResult         *res,
							  GError              **error);

static void
g_file_enumerator_finalize (GObject *object)
{
  GFileEnumerator *enumerator;

  enumerator = G_FILE_ENUMERATOR (object);
  
  if (!enumerator->priv->closed)
    g_file_enumerator_close (enumerator, NULL, NULL);

  if (G_OBJECT_CLASS (g_file_enumerator_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_enumerator_parent_class)->finalize) (object);
}

static void
g_file_enumerator_class_init (GFileEnumeratorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GFileEnumeratorPrivate));
  
  gobject_class->finalize = g_file_enumerator_finalize;

  klass->next_files_async = g_file_enumerator_real_next_files_async;
  klass->next_files_finish = g_file_enumerator_real_next_files_finish;
  klass->close_async = g_file_enumerator_real_close_async;
  klass->close_finish = g_file_enumerator_real_close_finish;
}

static void
g_file_enumerator_init (GFileEnumerator *enumerator)
{
  enumerator->priv = G_TYPE_INSTANCE_GET_PRIVATE (enumerator,
						  G_TYPE_FILE_ENUMERATOR,
						  GFileEnumeratorPrivate);
}

/**
 * g_file_enumerator_next_file:
 * @enumerator: a #GFileEnumerator.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Returns information for the next file in the enumerated object.
 * Will block until the information is available.
 *
 * On error, returns %NULL and sets @error to the error. If the
 * enumerator is at the end, %NULL will be returned and @error will
 * be unset.
 *
 * Return value: A #GFileInfo or %NULL on error or end of enumerator
 **/
GFileInfo *
g_file_enumerator_next_file (GFileEnumerator *enumerator,
			     GCancellable *cancellable,
			     GError **error)
{
  GFileEnumeratorClass *class;
  GFileInfo *info;
  
  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), NULL);
  g_return_val_if_fail (enumerator != NULL, NULL);
  
  if (enumerator->priv->closed)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
		   _("Enumerator is closed"));
      return NULL;
    }

  if (enumerator->priv->pending)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("File enumerator has outstanding operation"));
      return NULL;
    }

  if (enumerator->priv->outstanding_error)
    {
      g_propagate_error (error, enumerator->priv->outstanding_error);
      enumerator->priv->outstanding_error = NULL;
      return NULL;
    }
  
  class = G_FILE_ENUMERATOR_GET_CLASS (enumerator);

  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  enumerator->priv->pending = TRUE;
  info = (* class->next_file) (enumerator, cancellable, error);
  enumerator->priv->pending = FALSE;

  if (cancellable)
    g_pop_current_cancellable (cancellable);
  
  return info;
}
  
/**
 * g_file_enumerator_close:
 * @enumerator: a #GFileEnumerator.
 * @cancellable: optional #GCancellable object, %NULL to ignore. 
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Releases all resources used by this enumerator, making the
 * enumerator return %G_IO_ERROR_CLOSED on all calls.
 *
 * This will be automatically called when the last reference
 * is dropped, but you might want to call make sure resources
 * are released as early as possible.
 *
 * Return value: #TRUE on success or #FALSE on error.
 **/
gboolean
g_file_enumerator_close (GFileEnumerator *enumerator,
			 GCancellable *cancellable,
			 GError **error)
{
  GFileEnumeratorClass *class;

  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), FALSE);
  g_return_val_if_fail (enumerator != NULL, FALSE);
  
  class = G_FILE_ENUMERATOR_GET_CLASS (enumerator);

  if (enumerator->priv->closed)
    return TRUE;
  
  if (enumerator->priv->pending)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("File enumerator has outstanding operation"));
      return FALSE;
    }

  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  enumerator->priv->pending = TRUE;
  (* class->close) (enumerator, cancellable, error);
  enumerator->priv->pending = FALSE;
  enumerator->priv->closed = TRUE;

  if (cancellable)
    g_pop_current_cancellable (cancellable);
  
  return TRUE;
}

static void
next_async_callback_wrapper (GObject *source_object,
			     GAsyncResult *res,
			     gpointer user_data)
{
  GFileEnumerator *enumerator = G_FILE_ENUMERATOR (source_object);

  enumerator->priv->pending = FALSE;
  if (enumerator->priv->outstanding_callback)
    (*enumerator->priv->outstanding_callback) (source_object, res, user_data);
  g_object_unref (enumerator);
}

/**
 * g_file_enumerator_next_files_async:
 * @enumerator: a #GFileEnumerator.
 * @num_files: the number of file info objects to request
 * @io_priority: the io priority of the request. the io priority of the request
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied
 * @user_data: the user_data to pass to callback function 
 *
 * Request information for a number of files from the enumerator asynchronously.
 * When all i/o for the operation is finished the @callback will be called with
 * the requested information.
 *
 * The callback can be called with less than @num_files files in case of error
 * or at the end of the enumerator. In case of a partial error the callback will
 * be called with any succeeding items and no error, and on the next request the
 * error will be reported. If a request is cancelled the callback will be called
 * with %G_IO_ERROR_CANCELLED.
 *
 * During an async request no other sync and async calls are allowed, and will
 * result in %G_IO_ERROR_PENDING errors. 
 *
 * Any outstanding i/o request with higher priority (lower numerical value) will
 * be executed before an outstanding request with lower priority. Default
 * priority is %G_PRIORITY_DEFAULT.
 **/
void
g_file_enumerator_next_files_async  (GFileEnumerator                *enumerator,
				     int                             num_files,
				     int                             io_priority,
				     GCancellable                   *cancellable,
				     GAsyncReadyCallback             callback,
				     gpointer                        user_data)
{
  GFileEnumeratorClass *class;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_IS_FILE_ENUMERATOR (enumerator));
  g_return_if_fail (enumerator != NULL);
  g_return_if_fail (num_files >= 0);

  if (num_files == 0)
    {
      simple = g_simple_async_result_new (G_OBJECT (enumerator),
					  callback,
					  user_data,
					  g_file_enumerator_next_files_async);
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      return;
    }
  
  if (enumerator->priv->closed)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (enumerator),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_CLOSED,
					   _("File enumerator is already closed"));
      return;
    }
  
  if (enumerator->priv->pending)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (enumerator),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_PENDING,
					   _("File enumerator has outstanding operation"));
      return;
    }

  class = G_FILE_ENUMERATOR_GET_CLASS (enumerator);
  
  enumerator->priv->pending = TRUE;
  enumerator->priv->outstanding_callback = callback;
  g_object_ref (enumerator);
  (* class->next_files_async) (enumerator, num_files, io_priority, cancellable, 
			       next_async_callback_wrapper, user_data);
}

/**
 * g_file_enumerator_next_files_finish:
 * @enumerator: a #GFileEnumerator.
 * @result: a #GAsyncResult.
 * @error: a #GError location to store the error occuring, or %NULL to 
 * ignore.
 * 
 * 
 * 
 * Returns: 
 **/
GList *
g_file_enumerator_next_files_finish  (GFileEnumerator *enumerator,
				      GAsyncResult *result,
				      GError **error)
{
  GFileEnumeratorClass *class;
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), NULL);
  g_return_val_if_fail (G_IS_ASYNC_RESULT (result), NULL);
  
  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
	return NULL;
      
      /* Special case read of 0 files */
      if (g_simple_async_result_get_source_tag (simple) == g_file_enumerator_next_files_async)
	return NULL;
    }
  
  class = G_FILE_ENUMERATOR_GET_CLASS (enumerator);
  return class->next_files_finish (enumerator, result, error);
}

static void
close_async_callback_wrapper (GObject *source_object,
			      GAsyncResult *res,
			      gpointer user_data)
{
  GFileEnumerator *enumerator = G_FILE_ENUMERATOR (source_object);
  
  enumerator->priv->pending = FALSE;
  enumerator->priv->closed = TRUE;
  if (enumerator->priv->outstanding_callback)
    (*enumerator->priv->outstanding_callback) (source_object, res, user_data);
  g_object_unref (enumerator);
}

/**
 * g_file_enumerator_close_async:
 * @enumerator: a #GFileEnumerator.
 * @io_priority: the io priority of the request. the io priority of the request
 * @cancellable: optional #GCancellable object, %NULL to ignore. 
 * @callback: callback to call when the request is satisfied
 * @user_data: the user_data to pass to callback function 
 * 
 **/
void
g_file_enumerator_close_async (GFileEnumerator                *enumerator,
			       int                             io_priority,
			       GCancellable                   *cancellable,
			       GAsyncReadyCallback             callback,
			       gpointer                        user_data)
{
  GFileEnumeratorClass *class;

  g_return_if_fail (G_IS_FILE_ENUMERATOR (enumerator));

  if (enumerator->priv->closed)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (enumerator),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_CLOSED,
					   _("File enumerator is already closed"));
      return;
    }
  
  if (enumerator->priv->pending)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (enumerator),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_PENDING,
					   _("File enumerator has outstanding operation"));
      return;
    }

  class = G_FILE_ENUMERATOR_GET_CLASS (enumerator);
  
  enumerator->priv->pending = TRUE;
  enumerator->priv->outstanding_callback = callback;
  g_object_ref (enumerator);
  (* class->close_async) (enumerator, io_priority, cancellable,
			  close_async_callback_wrapper, user_data);
}

/**
 * g_file_enumerator_close_finish:
 * @enumerator: a #GFileEnumerator.
 * @result: a #GAsyncResult.
 * @error: a #GError location to store the error occuring, or %NULL to 
 * ignore.
 * 
 * 
 * 
 * Returns: %TRUE if the close operation has finished successfully.
 **/
gboolean
g_file_enumerator_close_finish (GFileEnumerator                *enumerator,
				GAsyncResult                   *result,
				GError                        **error)
{
  GSimpleAsyncResult *simple;
  GFileEnumeratorClass *class;

  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), FALSE);
  g_return_val_if_fail (G_IS_ASYNC_RESULT (result), FALSE);
  
  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
	return FALSE;
    }
  
  class = G_FILE_ENUMERATOR_GET_CLASS (enumerator);
  return class->close_finish (enumerator, result, error);
}

/**
 * g_file_enumerator_is_closed:
 * @enumerator: a #GFileEnumerator.
 * 
 * Returns: %TRUE if the @enumerator is closed.
 **/
gboolean
g_file_enumerator_is_closed (GFileEnumerator *enumerator)
{
  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), TRUE);
  
  return enumerator->priv->closed;
}

/**
 * g_file_enumerator_has_pending:
 * @enumerator: a #GFileEnumerator.
 * 
 * Returns: %TRUE if the @enumerator has pending operations.
 **/
gboolean
g_file_enumerator_has_pending (GFileEnumerator *enumerator)
{
  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), TRUE);
  
  return enumerator->priv->pending;
}

/**
 * g_file_enumerator_set_pending:
 * @enumerator: a #GFileEnumerator.
 * @pending: a boolean value.
 * 
 **/
void
g_file_enumerator_set_pending (GFileEnumerator              *enumerator,
			       gboolean                   pending)
{
  g_return_if_fail (G_IS_FILE_ENUMERATOR (enumerator));
  
  enumerator->priv->pending = pending;
}

typedef struct {
  int                num_files;
  GList             *files;
} NextAsyncOp;

static void
next_files_thread (GSimpleAsyncResult *res,
		   GObject *object,
		   GCancellable *cancellable)
{
  NextAsyncOp *op;
  GFileEnumeratorClass *class;
  GError *error = NULL;
  GFileInfo *info;
  GFileEnumerator *enumerator;
  int i;

  enumerator = G_FILE_ENUMERATOR (object);
  op = g_simple_async_result_get_op_res_gpointer (res);

  class = G_FILE_ENUMERATOR_GET_CLASS (object);

  for (i = 0; i < op->num_files; i++)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, &error))
	info = NULL;
      else
	info = class->next_file (enumerator, cancellable, &error);
      
      if (info == NULL)
	{
	  /* If we get an error after first file, return that on next operation */
	  if (error != NULL && i > 0)
	    {
	      if (error->domain == G_IO_ERROR &&
		  error->code == G_IO_ERROR_CANCELLED)
		g_error_free (error); /* Never propagate cancel errors to other call */
	      else
		enumerator->priv->outstanding_error = error;
	      error = NULL;
	    }
	      
	  break;
	}
      else
	op->files = g_list_prepend (op->files, info);
    }
}


static void
g_file_enumerator_real_next_files_async (GFileEnumerator                *enumerator,
					 int                             num_files,
					 int                             io_priority,
					 GCancellable                   *cancellable,
					 GAsyncReadyCallback             callback,
					 gpointer                        user_data)
{
  GSimpleAsyncResult *res;
  NextAsyncOp *op;

  op = g_new0 (NextAsyncOp, 1);

  op->num_files = num_files;
  op->files = NULL;

  res = g_simple_async_result_new (G_OBJECT (enumerator), callback, user_data, g_file_enumerator_real_next_files_async);
  g_simple_async_result_set_op_res_gpointer (res, op, g_free);
  
  g_simple_async_result_run_in_thread (res, next_files_thread, io_priority, cancellable);
  g_object_unref (res);
}

static GList *
g_file_enumerator_real_next_files_finish (GFileEnumerator                *enumerator,
					  GAsyncResult                   *result,
					  GError                        **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  NextAsyncOp *op;

  g_assert (g_simple_async_result_get_source_tag (simple) == 
	    g_file_enumerator_real_next_files_async);

  op = g_simple_async_result_get_op_res_gpointer (simple);

  return op->files;
}

static void
close_async_thread (GSimpleAsyncResult *res,
		    GObject *object,
		    GCancellable *cancellable)
{
  GFileEnumeratorClass *class;
  GError *error = NULL;
  gboolean result;

  /* Auto handling of cancelation disabled, and ignore
     cancellation, since we want to close things anyway, although
     possibly in a quick-n-dirty way. At least we never want to leak
     open handles */
  
  class = G_FILE_ENUMERATOR_GET_CLASS (object);
  result = class->close (G_FILE_ENUMERATOR (object), cancellable, &error);
  if (!result)
    {
      g_simple_async_result_set_from_error (res, error);
      g_error_free (error);
    }
}


static void
g_file_enumerator_real_close_async (GFileEnumerator      *enumerator,
				    int                   io_priority,
				    GCancellable         *cancellable,
				    GAsyncReadyCallback   callback,
				    gpointer              user_data)
{
  GSimpleAsyncResult *res;
  
  res = g_simple_async_result_new (G_OBJECT (enumerator),
				   callback,
				   user_data,
				   g_file_enumerator_real_close_async);

  g_simple_async_result_set_handle_cancellation (res, FALSE);
  
  g_simple_async_result_run_in_thread (res,
				       close_async_thread,
				       io_priority,
				       cancellable);
  g_object_unref (res);
}

static gboolean
g_file_enumerator_real_close_finish      (GFileEnumerator      *enumerator,
					  GAsyncResult         *result,
					  GError              **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  g_assert (g_simple_async_result_get_source_tag (simple) == 
	    g_file_enumerator_real_close_async);
  return TRUE;
}