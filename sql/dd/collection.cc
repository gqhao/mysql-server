/* Copyright (c) 2014, 2016 Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#include "dd/collection.h"

#include "dd/impl/object_key.h"         // Needed for destructor
#include "dd/impl/raw/raw_record_set.h" // dd::Raw_record_set
#include "dd/impl/raw/raw_table.h"      // dd::Raw_table

#include "dd/impl/types/abstract_table_impl.h"
#include "dd/impl/types/column_impl.h"
#include "dd/impl/types/column_type_element_impl.h"
#include "dd/impl/types/foreign_key_impl.h"
#include "dd/impl/types/foreign_key_element_impl.h"
#include "dd/impl/types/index_impl.h"
#include "dd/impl/types/index_element_impl.h"
#include "dd/impl/types/parameter_impl.h"
#include "dd/impl/types/parameter_type_element_impl.h"
#include "dd/impl/types/partition_impl.h"
#include "dd/impl/types/partition_index_impl.h"
#include "dd/impl/types/partition_value_impl.h"
#include "dd/impl/types/routine_impl.h"
#include "dd/impl/types/table_impl.h"
#include "dd/impl/types/tablespace_impl.h"
#include "dd/impl/types/tablespace_file_impl.h"
#include "dd/impl/types/view_impl.h"
#include "dd/impl/types/view_table_impl.h"
#include "dd/impl/types/trigger_impl.h"

#include <memory>     // std::unique_ptr

namespace dd {


template <typename T>
T& Collection<T>::Collection_iterator::operator*()
{
  // Dereferencing an invalid iterator is prohibited.
  DBUG_ASSERT(m_current != m_array->end());

  // Need a non-tmp placeholder of correct type since reference is returned.
  m_current_obj= *m_current;
  return m_current_obj;
}


template <typename T>
const typename Collection<T>::abstract_type*&
Collection<T>::Collection_const_iterator::operator*()
{
  // Dereferencing an invalid iterator is prohibited.
  DBUG_ASSERT(m_current != m_array->end());

  // Need a non-tmp placeholder of correct type since reference is returned.
  m_current_obj= *m_current;
  return m_current_obj;
}


template <typename T>
void Collection<T>::clear_all_items()
{
  delete_container_pointers(m_items);
  delete_container_pointers(m_removed_items);
}


template <typename T>
void Collection<T>::clear_removed_items()
{
  delete_container_pointers(m_removed_items);
}


// Doxygen gets confused by this function.

/**
 @cond
*/

template <typename T>
void Collection<T>::remove(typename Collection<T>::impl_type *item)
{
  if (std::find(m_items.begin(), m_items.end(), item) != m_items.end())
  {
    m_removed_items.push_back(item);

    // Remove items using the "erase-remove" idiom.
    m_items.erase(
      std::remove(m_items.begin(), m_items.end(), item),
      m_items.end());

    renumerate_items();
  }
}


/**
 @endcond
*/

template <typename T>
static bool item_compare(const T *a,
                         const T *b)
{ return a->ordinal_position() < b->ordinal_position(); }


template <typename T>
template <typename Parent_item>
bool Collection<T>::restore_items(
  Parent_item *parent,
  Open_dictionary_tables_ctx *otx,
  Raw_table *table,
  Object_key *key)
{
  DBUG_ENTER("Collection::restore_items");

  // NOTE: if this assert is firing, that means the table was not registered
  // for that transaction. Use Open_dictionary_tables_ctx::register_tables().
  DBUG_ASSERT(table);

  DBUG_ASSERT(empty());

  std::unique_ptr<Object_key> key_holder(key);

  std::unique_ptr<Raw_record_set> rs;
  if (table->open_record_set(key, rs))
    DBUG_RETURN(true);

  // Process records.

  Raw_record *r= rs->current_record();
  while (r)
  {
    Collection<T>::impl_type *item= Collection<T>::impl_type::restore_item(parent);
    item->set_ordinal_position(static_cast<uint>(m_items.size() + 1));
    m_items.push_back(item);

    if (item->restore_attributes(*r) ||
        item->restore_children(otx) ||
        item->validate() ||
        rs->next(r))
    {
      clear_all_items();
      DBUG_RETURN(true);
    }

  }

  // The record fetched from DB may not be ordered based on ordinal position,
  // since some elements store their ordinal position persistently.
  // So we need to sort the elements in m_item based on ordinal position.
  std::sort(m_items.begin(), m_items.end(), item_compare<Collection<T>::impl_type>);

  DBUG_RETURN(false);
}


template <typename T>
bool Collection<T>::store_items(Open_dictionary_tables_ctx *otx)
{
  DBUG_ENTER("Collection::store_items");

  if (empty())
    DBUG_RETURN(false);

  // Drop items from m_removed_items.

  for (Weak_object_impl *removed : m_removed_items)
  {
    if (removed->validate() || removed->drop(otx))
      DBUG_RETURN(true);
  }

  delete_container_pointers(m_removed_items);

  // Add new items and update existing if needed.

  for (Collection<T>::impl_type *item : m_items)
  {
    if (item->validate() || item->store(otx))
      DBUG_RETURN(true);
  }

  DBUG_RETURN(false);
}


template <typename T>
bool Collection<T>::drop_items(Open_dictionary_tables_ctx *otx,
                               Raw_table *table,
                               Object_key *key) const
{
  DBUG_ENTER("Collection::drop_items");

  // Make sure key gets deleted
  std::unique_ptr<Object_key> key_holder(key);

  if (empty())
    DBUG_RETURN(false);

  // Drop items

  for (const Collection<T>::impl_type *item : m_items)
  {
    if (item->drop_children(otx))
      DBUG_RETURN(true);
  }

  std::unique_ptr<Raw_record_set> rs;
  if (table->open_record_set(key, rs))
    DBUG_RETURN(true);

  // Process records.

  Raw_record *r= rs->current_record();
  while (r)
  {
    // Drop the item record from DD table
    if (r->drop())
      DBUG_RETURN(true);

    if (rs->next(r))
      DBUG_RETURN(true);
  }

  DBUG_RETURN(false);
}


template <typename T>
const typename Collection<T>::abstract_type* Collection<T>::at(size_t n) const
{
  DBUG_ASSERT(n < size());
  return m_items[n];
}


template <typename T>
template <typename Parent_item>
void Collection<T>::deep_copy(const Collection<T> &src, Parent_item *parent)
{
  m_items.reserve(src.m_items.size());
  for (const Collection<T>::impl_type *item : src.m_items)
    m_items.push_back(Collection<T>::impl_type::clone(*item, parent));
}

///////////////////////////////////////////////////////////////////////////

// The explicit instantiation of the template members below
// is not handled well by doxygen, so we enclose this in a
// cond/endcon block. Documenting these does not add much
// value anyway, if the member definitions were in a header
// file, the compiler would do the instantiation for us.

/**
 @cond
*/

template Column*&
Collection<Column*>::Collection_iterator::operator*();
template Column_type_element*&
Collection<Column_type_element*>::Collection_iterator::operator*();
template Index*&
Collection<Index*>::Collection_iterator::operator*();
template Index_element*&
Collection<Index_element*>::Collection_iterator::operator*();
template Parameter_type_element*&
Collection<Parameter_type_element*>::Collection_iterator::operator*();
template Partition*&
Collection<Partition*>::Collection_iterator::operator*();
template Tablespace_file*&
Collection<Tablespace_file*>::Collection_iterator::operator*();
template Trigger*&
Collection<Trigger*>::Collection_iterator::operator*();

template const Column*&
Collection<Column*>::Collection_const_iterator::operator*();
template const Column_type_element*&
Collection<Column_type_element*>::Collection_const_iterator::operator*();
template const Foreign_key*&
Collection<Foreign_key*>::Collection_const_iterator::operator*();
template const Foreign_key_element*&
Collection<Foreign_key_element*>::Collection_const_iterator::operator*();
template const Index*&
Collection<Index*>::Collection_const_iterator::operator*();
template const Index_element*&
Collection<Index_element*>::Collection_const_iterator::operator*();
template const Parameter*&
Collection<Parameter*>::Collection_const_iterator::operator*();
template const Parameter_type_element*&
Collection<Parameter_type_element*>::Collection_const_iterator::operator*();
template const Partition*&
Collection<Partition*>::Collection_const_iterator::operator*();
template const Partition_index*&
Collection<Partition_index*>::Collection_const_iterator::operator*();
template const Partition_value*&
Collection<Partition_value*>::Collection_const_iterator::operator*();
template const Tablespace_file*&
Collection<Tablespace_file*>::Collection_const_iterator::operator*();
template const View_table*&
Collection<View_table*>::Collection_const_iterator::operator*();
template const Trigger*&
Collection<Trigger*>::Collection_const_iterator::operator*();

template bool Collection<Column*>::
restore_items<Abstract_table_impl>(Abstract_table_impl*,
                                   Open_dictionary_tables_ctx*,
                                   Raw_table*, Object_key*);
template bool Collection<Column_type_element*>::
restore_items<Column_impl>(Column_impl*,
                          Open_dictionary_tables_ctx*,
                          Raw_table*, Object_key*);
template bool Collection<Foreign_key*>::
restore_items<Table_impl>(Table_impl*,
                          Open_dictionary_tables_ctx*,
                          Raw_table*, Object_key*);
template bool Collection<Foreign_key_element*>::
restore_items<Foreign_key_impl>(Foreign_key_impl*,
                                Open_dictionary_tables_ctx*,
                                Raw_table*, Object_key*);
template bool Collection<Index*>::
restore_items<Table_impl>(Table_impl*,
                          Open_dictionary_tables_ctx*,
                          Raw_table*, Object_key*);
template bool Collection<Index_element*>::
restore_items<Index_impl>(Index_impl*,
                          Open_dictionary_tables_ctx*,
                          Raw_table*, Object_key*);
template bool Collection<Parameter*>::
restore_items<Routine_impl>(Routine_impl*,
                            Open_dictionary_tables_ctx*,
                            Raw_table*, Object_key*);
template bool Collection<Parameter_type_element*>::
restore_items<Parameter_impl>(Parameter_impl*,
                              Open_dictionary_tables_ctx*,
                              Raw_table*, Object_key*);
template bool Collection<Partition*>::
restore_items<Table_impl>(Table_impl*,
                          Open_dictionary_tables_ctx*,
                          Raw_table*, Object_key*);
template bool Collection<Partition_index*>::
restore_items<Partition_impl>(Partition_impl*,
                              Open_dictionary_tables_ctx*,
                              Raw_table*, Object_key*);
template bool Collection<Partition_value*>::
restore_items<Partition_impl>(Partition_impl*,
                              Open_dictionary_tables_ctx*,
                              Raw_table*, Object_key*);
template bool Collection<Tablespace_file*>::
restore_items<Tablespace_impl>(Tablespace_impl*,
                               Open_dictionary_tables_ctx*,
                               Raw_table*, Object_key*);
template bool Collection<View_table*>::
restore_items<View_impl>(View_impl*,
                         Open_dictionary_tables_ctx*,
                         Raw_table*, Object_key*);
template bool Collection<Trigger*>::
restore_items<Table_impl>(Table_impl*,
                          Open_dictionary_tables_ctx*,
                          Raw_table*, Object_key*);

template bool Collection<Column*>::
store_items(Open_dictionary_tables_ctx*);
template bool Collection<Column_type_element*>::
store_items(Open_dictionary_tables_ctx*);
template bool Collection<Foreign_key*>::
store_items(Open_dictionary_tables_ctx*);
template bool Collection<Foreign_key_element*>::
store_items(Open_dictionary_tables_ctx*);
template bool Collection<Index*>::
store_items(Open_dictionary_tables_ctx*);
template bool Collection<Index_element*>::
store_items(Open_dictionary_tables_ctx*);
template bool Collection<Parameter*>::
store_items(Open_dictionary_tables_ctx*);
template bool Collection<Parameter_type_element*>::
store_items(Open_dictionary_tables_ctx*);
template bool Collection<Partition*>::
store_items(Open_dictionary_tables_ctx*);
template bool Collection<Partition_index*>::
store_items(Open_dictionary_tables_ctx*);
template bool Collection<Partition_value*>::
store_items(Open_dictionary_tables_ctx*);
template bool Collection<Tablespace_file*>::
store_items(Open_dictionary_tables_ctx*);
template bool Collection<View_table*>::
store_items(Open_dictionary_tables_ctx*);
template bool Collection<Trigger*>::
store_items(Open_dictionary_tables_ctx*);

template bool Collection<Column*>::
drop_items(Open_dictionary_tables_ctx*, Raw_table*, Object_key*) const;
template bool Collection<Column_type_element*>::
drop_items(Open_dictionary_tables_ctx*, Raw_table*, Object_key*) const;
template bool Collection<Foreign_key*>::
drop_items(Open_dictionary_tables_ctx*, Raw_table*, Object_key*) const;
template bool Collection<Foreign_key_element*>::
drop_items(Open_dictionary_tables_ctx*, Raw_table*, Object_key*) const;
template bool Collection<Index*>::
drop_items(Open_dictionary_tables_ctx*, Raw_table*, Object_key*) const;
template bool Collection<Index_element*>::
drop_items(Open_dictionary_tables_ctx*, Raw_table*, Object_key*) const;
template bool Collection<Parameter_type_element*>::
drop_items(Open_dictionary_tables_ctx*, Raw_table*, Object_key*) const;
template bool Collection<Parameter*>::
drop_items(Open_dictionary_tables_ctx*, Raw_table*, Object_key*) const;
template bool Collection<Partition*>::
drop_items(Open_dictionary_tables_ctx*, Raw_table*, Object_key*) const;
template bool Collection<Partition_index*>::
drop_items(Open_dictionary_tables_ctx*, Raw_table*, Object_key*) const;
template bool Collection<Partition_value*>::
drop_items(Open_dictionary_tables_ctx*, Raw_table*, Object_key*) const;
template bool Collection<Tablespace_file*>::
drop_items(Open_dictionary_tables_ctx*, Raw_table*, Object_key*) const;
template bool Collection<View_table*>::
drop_items(Open_dictionary_tables_ctx*, Raw_table*, Object_key*) const;
template bool Collection<Trigger*>::
drop_items(Open_dictionary_tables_ctx*, Raw_table*, Object_key*) const;

template void Collection<Column*>::clear_all_items();
template void Collection<Column_type_element*>::clear_all_items();
template void Collection<Index*>::clear_all_items();
template void Collection<Index_element*>::clear_all_items();
template void Collection<Foreign_key*>::clear_all_items();
template void Collection<Foreign_key_element*>::clear_all_items();
template void Collection<Parameter*>::clear_all_items();
template void Collection<Parameter_type_element*>::clear_all_items();
template void Collection<Partition*>::clear_all_items();
template void Collection<Partition_index*>::clear_all_items();
template void Collection<Partition_value*>::clear_all_items();
template void Collection<Tablespace_file*>::clear_all_items();
template void Collection<View_table*>::clear_all_items();
template void Collection<Trigger*>::clear_all_items();

template void Collection<Column*>::clear_removed_items();
template void Collection<Column_type_element*>::clear_removed_items();
template void Collection<Index*>::clear_removed_items();
template void Collection<Index_element*>::clear_removed_items();
template void Collection<Foreign_key*>::clear_removed_items();
template void Collection<Foreign_key_element*>::clear_removed_items();
template void Collection<Parameter*>::clear_removed_items();
template void Collection<Parameter_type_element*>::clear_removed_items();
template void Collection<Partition*>::clear_removed_items();
template void Collection<Partition_index*>::clear_removed_items();
template void Collection<Partition_value*>::clear_removed_items();
template void Collection<Tablespace_file*>::clear_removed_items();
template void Collection<View_table*>::clear_removed_items();
template void Collection<Trigger*>::clear_removed_items();

template void Collection<Column*>::remove(Column_impl*);
template void Collection<Column_type_element*>::remove(Column_type_element_impl*);
template void Collection<Foreign_key*>::remove(Foreign_key_impl*);
template void Collection<Index*>::remove(Index_impl*);
template void Collection<Index_element*>::remove(Index_element_impl*);
template void Collection<Foreign_key_element*>::remove(Foreign_key_element_impl*);
template void Collection<Parameter*>::remove(Parameter_impl*);
template void Collection<Parameter_type_element*>::remove(Parameter_type_element_impl*);
template void Collection<Partition*>::remove(Partition_impl*);
template void Collection<Partition_index*>::remove(Partition_index_impl*);
template void Collection<Partition_value*>::remove(Partition_value_impl*);
template void Collection<Tablespace_file*>::remove(Tablespace_file_impl*);
template void Collection<View_table*>::remove(View_table_impl*);
template void Collection<Trigger*>::remove(Trigger_impl*);

template const Collection<Column*>::abstract_type*
Collection<Column*>::at(size_t n) const;
template const Collection<Foreign_key*>::abstract_type*
Collection<Foreign_key*>::at(size_t n) const;
template const Collection<Foreign_key_element*>::abstract_type*
Collection<Foreign_key_element*>::at(size_t n) const;
template const Collection<Index*>::abstract_type*
Collection<Index*>::at(size_t n) const;
template const Collection<Index_element*>::abstract_type*
Collection<Index_element*>::at(size_t n) const;

template void Collection<Column*>::
deep_copy<Abstract_table_impl>(Collection<Column*> const&, Abstract_table_impl*);
template void Collection<Column_type_element*>::
deep_copy<Column_impl>(Collection<Column_type_element*> const&, Column_impl*);
template void Collection<Foreign_key*>::
deep_copy<Table_impl>(Collection<Foreign_key*> const&, Table_impl*);
template void Collection<Foreign_key_element*>::
deep_copy<Foreign_key_impl>(Collection<Foreign_key_element*> const&, Foreign_key_impl*);
template void Collection<Index*>::
deep_copy<Table_impl>(Collection<Index*> const&, Table_impl*);
template void Collection<Index_element*>::
deep_copy<Index_impl>(Collection<Index_element*> const&, Index_impl*);
template void Collection<Parameter*>::
deep_copy<Routine_impl>(Collection<Parameter*> const&, Routine_impl*);
template void Collection<Parameter_type_element*>::
deep_copy<Parameter_impl>(Collection<Parameter_type_element*> const&, Parameter_impl*);
template void Collection<Partition*>::
deep_copy<Table_impl>(Collection<Partition*> const&, Table_impl*);
template void Collection<Partition_index*>::
deep_copy<Partition_impl>(Collection<Partition_index*> const&, Partition_impl*);
template void Collection<Partition_value*>::
deep_copy<Partition_impl>(Collection<Partition_value*> const&, Partition_impl*);
template void Collection<Tablespace_file*>::
deep_copy<Tablespace_impl>(Collection<Tablespace_file*> const&, Tablespace_impl*);
template void Collection<View_table*>::
deep_copy<View_impl>(Collection<View_table*> const&, View_impl*);
template void Collection<Trigger*>::
deep_copy<Table_impl>(Collection<Trigger*> const&, Table_impl*);

/**
 @endcond
*/

} // namespace dd
