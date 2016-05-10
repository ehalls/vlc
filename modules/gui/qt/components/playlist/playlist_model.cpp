/*****************************************************************************
 * playlist_model.cpp : Manage playlist model
 ****************************************************************************
 * Copyright (C) 2006-2011 the VideoLAN team
 * $Id$
 *
 * Authors: Clément Stenac <zorglub@videolan.org>
 *          Ilkka Ollakkka <ileoo (at) videolan dot org>
 *          Jakob Leben <jleben@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "qt.hpp"
#include "components/playlist/playlist_model.hpp"
#include "input_manager.hpp"                            /* THEMIM */
#include "util/qt_dirs.hpp"
#include "recents.hpp"                                  /* Open:: */

#include <vlc_intf_strings.h>                           /* I_DIR */

#include "sorting.h"

#include <assert.h>
#include <QFont>
#include <QAction>

/*************************************************************************
 * Playlist model implementation
 *************************************************************************/

PLModel::PLModel( playlist_t *_p_playlist,  /* THEPL */
                  intf_thread_t *_p_intf,   /* main Qt p_intf */
                  playlist_item_t * p_root,
                  QObject *parent )         /* Basic Qt parent */
                  : VLCModel( _p_intf, parent )
{
    p_playlist        = _p_playlist;

    rootItem          = NULL; /* PLItem rootItem, will be set in rebuild( ) */
    latestSearch      = QString();

    rebuild( p_root );
    DCONNECT( THEMIM->getIM(), metaChanged( input_item_t *),
              this, processInputItemUpdate( input_item_t *) );
    DCONNECT( THEMIM, inputChanged( bool ),
              this, processInputItemUpdate( ) );
    CONNECT( THEMIM, playlistItemAppended( int, int ),
             this, processItemAppend( int, int ) );
    CONNECT( THEMIM, playlistItemRemoved( int ),
             this, processItemRemoval( int ) );
}

PLModel::~PLModel()
{
    delete rootItem;
}

Qt::DropActions PLModel::supportedDropActions() const
{
    return Qt::CopyAction | Qt::MoveAction;
}

Qt::ItemFlags PLModel::flags( const QModelIndex &index ) const
{
    Qt::ItemFlags flags = QAbstractItemModel::flags( index );

    const PLItem *item = index.isValid() ? getItem( index ) : rootItem;

    if( canEdit() )
    {
        PL_LOCK;
        playlist_item_t *plItem =
            playlist_ItemGetById( p_playlist, item->i_playlist_id );

        if ( plItem && ( plItem->i_children > -1 ) )
            flags |= Qt::ItemIsDropEnabled;

        PL_UNLOCK;

    }
    flags |= Qt::ItemIsDragEnabled;

    return flags;
}

QStringList PLModel::mimeTypes() const
{
    QStringList types;
    types << "vlc/qt-input-items";
    return types;
}

bool modelIndexLessThen( const QModelIndex &i1, const QModelIndex &i2 )
{
    if( !i1.isValid() || !i2.isValid() ) return false;
    PLItem *item1 = static_cast<PLItem*>( i1.internalPointer() );
    PLItem *item2 = static_cast<PLItem*>( i2.internalPointer() );
    if( item1->hasSameParent( item2 ) ) return i1.row() < i2.row();
    else return *item1 < *item2;
}

QMimeData *PLModel::mimeData( const QModelIndexList &indexes ) const
{
    PlMimeData *plMimeData = new PlMimeData();
    QModelIndexList list;

    foreach( const QModelIndex &index, indexes ) {
        if( index.isValid() && index.column() == 0 )
            list.append(index);
    }

    qSort(list.begin(), list.end(), modelIndexLessThen);

    AbstractPLItem *item = NULL;
    foreach( const QModelIndex &index, list ) {
        if( item )
        {
            AbstractPLItem *testee = getItem( index );
            while( testee->parent() )
            {
                if( testee->parent() == item ||
                    testee->parent() == item->parent() ) break;
                testee = testee->parent();
            }
            if( testee->parent() == item ) continue;
            item = getItem( index );
        }
        else
            item = getItem( index );

        plMimeData->appendItem( static_cast<PLItem*>(item)->inputItem() );
    }

    return plMimeData;
}

/* Drop operation */
bool PLModel::dropMimeData( const QMimeData *data, Qt::DropAction action,
        int row, int, const QModelIndex &parent )
{
    bool copy = action == Qt::CopyAction;
    if( !copy && action != Qt::MoveAction )
        return true;

    const PlMimeData *plMimeData = qobject_cast<const PlMimeData*>( data );
    if( plMimeData )
    {
        if( copy )
            dropAppendCopy( plMimeData, getItem( parent ), row );
        else
            dropMove( plMimeData, getItem( parent ), row );
    }
    return true;
}

void PLModel::dropAppendCopy( const PlMimeData *plMimeData, PLItem *target, int pos )
{
    PL_LOCK;

    playlist_item_t *p_parent =
        playlist_ItemGetByInput( p_playlist, target->inputItem() );
    if( !p_parent ) return;

    if( pos == -1 ) pos = PLAYLIST_END;

    QList<input_item_t*> inputItems = plMimeData->inputItems();

    foreach( input_item_t* p_input, inputItems )
    {
        playlist_item_t *p_item = playlist_ItemGetByInput( p_playlist, p_input );
        if( !p_item ) continue;
        pos = playlist_NodeAddCopy( p_playlist, p_item, p_parent, pos );
    }

    PL_UNLOCK;
}

void PLModel::dropMove( const PlMimeData * plMimeData, PLItem *target, int row )
{
    QList<input_item_t*> inputItems = plMimeData->inputItems();
    QList<PLItem*> model_items;
    playlist_item_t **pp_items;
    pp_items = (playlist_item_t **)
               calloc( inputItems.count(), sizeof( playlist_item_t* ) );
    if ( !pp_items ) return;

    PL_LOCK;

    playlist_item_t *p_parent =
        playlist_ItemGetByInput( p_playlist, target->inputItem() );

    if( !p_parent || row > p_parent->i_children )
    {
        PL_UNLOCK;
        free( pp_items );
        return;
    }

    int new_pos = row == -1 ? p_parent->i_children : row;
    int model_pos = new_pos;
    int i = 0;

    foreach( input_item_t *p_input, inputItems )
    {
        playlist_item_t *p_item = playlist_ItemGetByInput( p_playlist, p_input );
        if( !p_item ) continue;

        PLItem *item = findByInputId( rootItem, p_input->i_id );
        if( !item ) continue;

        /* Better not try to move a node into itself.
           Abort the whole operation in that case,
           because it is ambiguous. */
        AbstractPLItem *climber = target;
        while( climber )
        {
            if( climber == item )
            {
                PL_UNLOCK;
                free( pp_items );
                return;
            }
            climber = climber->parent();
        }

        if( item->parent() == target &&
            target->children.indexOf( item ) < new_pos )
            model_pos--;

        model_items.append( item );
        pp_items[i] = p_item;
        i++;
    }

    if( model_items.isEmpty() )
    {
        PL_UNLOCK;
        free( pp_items );
        return;
    }

    playlist_TreeMoveMany( p_playlist, i, pp_items, p_parent, new_pos );

    PL_UNLOCK;

    foreach( PLItem *item, model_items )
        takeItem( item );

    insertChildren( target, model_items, model_pos );
    free( pp_items );
}

void PLModel::activateItem( const QModelIndex &index )
{
    assert( index.isValid() );
    const PLItem *item = getItem( index );
    assert( item );
    PL_LOCK;
    playlist_item_t *p_item = playlist_ItemGetById( p_playlist, item->i_playlist_id );
    activateItem( p_item );
    PL_UNLOCK;
}

/* Convenient overloaded private version of activateItem
 * Must be entered with PL lock */
void PLModel::activateItem( playlist_item_t *p_item )
{
    if( !p_item ) return;
    playlist_item_t *p_parent = p_item;
    while( p_parent )
    {
        if( p_parent->i_id == rootItem->id( PLAYLIST_ID ) ) break;
        p_parent = p_parent->p_parent;
    }
    if( p_parent )
        playlist_Control( p_playlist, PLAYLIST_VIEWPLAY, pl_Locked,
                p_parent, p_item );
}

/****************** Base model mandatory implementations *****************/
QVariant PLModel::data( const QModelIndex &index, const int role ) const
{
    if( !index.isValid() )
        return QVariant();

    switch( role )
    {

        case Qt::FontRole:
            return customFont;

        case Qt::DisplayRole:
        {
            PLItem *item = getItem( index );
            int metadata = columnToMeta( index.column() );
            if( metadata == COLUMN_END )
                return QVariant();

            QString returninfo;
            if( metadata == COLUMN_NUMBER )
            {
                returninfo = QString::number( index.row() + 1 );
            }
            else if( metadata == COLUMN_COVER )
            {
                QString artUrl;
                artUrl = InputManager::decodeArtURL( item->inputItem() );
                if( artUrl.isEmpty() )
                {
                    for( int i = 0; i < item->childCount(); i++ )
                    {
                        artUrl = InputManager::decodeArtURL( item->child( i )->inputItem() );
                        if( !artUrl.isEmpty() )
                            break;
                    }
                }
                return artUrl;
            }
            else
            {
                char *psz = psz_column_meta( item->inputItem(), metadata );
                returninfo = qfu( psz );
                free( psz );
            }

            return QVariant( returninfo );
        }

        case Qt::DecorationRole:
        {
            switch( columnToMeta(index.column()) )
            {
                case COLUMN_TITLE:
                {
                    PLItem *item = getItem( index );
                    /* Used to segfault here because i_type wasn't always initialized */
                    return QVariant( icons[item->inputItem()->i_type] );
                }
                case COLUMN_COVER:
                    /* !warn: changes tree item line height. Otherwise, override
                     * delegate's sizehint */
                    return getArtPixmap( index, QSize(16,16) );
                default:
                    break;
            }
            break;
        }

        case Qt::BackgroundRole:
            if( isCurrent( index ) )
                return QVariant( QBrush( Qt::gray ) );
            break;

        case CURRENT_ITEM_ROLE:
            return QVariant( isCurrent( index ) );

        case CURRENT_ITEM_CHILD_ROLE:
            return QVariant( isParent( index, currentIndex() ) );

        case LEAF_NODE_ROLE:
            return QVariant( isLeaf( index ) );

        default:
            break;
    }

    return QVariant();
}

bool PLModel::setData( const QModelIndex &index, const QVariant & value, int role )
{
    switch( role )
    {
    case Qt::FontRole:
        customFont = value.value<QFont>();
        return true;
    default:
        return VLCModel::setData( index, value, role );
    }
}

/* Seek from current index toward the top and see if index is one of parent nodes */
bool PLModel::isParent( const QModelIndex &index, const QModelIndex &current ) const
{
    if( !index.isValid() )
        return false;

    if( index == current )
        return true;

    if( !current.isValid() || !current.parent().isValid() )
        return false;

    return isParent( index, current.parent() );
}

bool PLModel::isLeaf( const QModelIndex &index ) const
{
    bool b_isLeaf = false;
    PL_LOCK;
    playlist_item_t *plItem =
        playlist_ItemGetById( p_playlist, itemId( index, PLAYLIST_ID ) );

    if( plItem )
        b_isLeaf = plItem->i_children == -1;
    PL_UNLOCK;
    return b_isLeaf;
}

PLItem* PLModel::getItem( const QModelIndex & index ) const
{
    PLItem *item = static_cast<PLItem *>( VLCModel::getItem( index ) );
    if ( item == NULL ) item = rootItem;
    return item;
}

QModelIndex PLModel::index( const int row, const int column, const QModelIndex &parent )
                  const
{
    PLItem *parentItem = parent.isValid() ? getItem( parent ) : rootItem;

    PLItem *childItem = static_cast<PLItem*>(parentItem->child( row ));
    if( childItem )
        return createIndex( row, column, childItem );
    else
        return QModelIndex();
}

QModelIndex PLModel::indexByPLID( const int i_plid, const int c ) const
{
    return index( findByPLId( rootItem, i_plid ), c );
}

QModelIndex PLModel::indexByInputItemID( const int i_inputitem_id, const int c ) const
{
    return index( findByInputId( rootItem, i_inputitem_id ), c );
}

QModelIndex PLModel::rootIndex() const
{
    return index( findByPLId( rootItem, rootItem->id( PLAYLIST_ID ) ), 0 );
}

bool PLModel::isTree() const
{
    return ( ( rootItem && rootItem->id( PLAYLIST_ID ) != p_playlist->p_playing->i_id )
             || var_InheritBool( p_intf, "playlist-tree" ) );
}

/* Return the index of a given item */
QModelIndex PLModel::index( PLItem *item, int column ) const
{
    if( !item ) return QModelIndex();
    AbstractPLItem *parent = item->parent();
    if( parent )
        return createIndex( parent->lastIndexOf( item ),
                            column, item );
    return QModelIndex();
}

QModelIndex PLModel::currentIndex() const
{
    input_thread_t *p_input_thread = THEMIM->getInput();
    if( !p_input_thread ) return QModelIndex();
    PLItem *item = findByInputId( rootItem, input_GetItem( p_input_thread )->i_id );
    return index( item, 0 );
}

QModelIndex PLModel::parent( const QModelIndex &index ) const
{
    if( !index.isValid() ) return QModelIndex();

    PLItem *childItem = getItem( index );
    if( !childItem )
    {
        msg_Err( p_playlist, "Item not found" );
        return QModelIndex();
    }

    PLItem *parentItem = static_cast<PLItem*>(childItem->parent());
    if( !parentItem || parentItem == rootItem ) return QModelIndex();
    if( !parentItem->parent() )
    {
        msg_Err( p_playlist, "No parent found, trying row 0. Please report this" );
        return createIndex( 0, 0, parentItem );
    }
    return createIndex(parentItem->row(), 0, parentItem);
}

int PLModel::rowCount( const QModelIndex &parent ) const
{
    PLItem *parentItem = parent.isValid() ? getItem( parent ) : rootItem;
    return parentItem->childCount();
}

/************************* Lookups *****************************/
PLItem *PLModel::findByPLId( PLItem *root, int i_plitemid ) const
{
    return findInner( root, i_plitemid, false );
}

PLItem *PLModel::findByInputId( PLItem *root, int i_input_itemid ) const
{
    PLItem *result = findInner( root, i_input_itemid, true );
    return result;
}

PLItem * PLModel::findInner( PLItem *root, int i_id, bool b_isinputid ) const
{
    if( !root ) return NULL;

    if( !b_isinputid && root->id( PLAYLIST_ID ) == i_id )
        return root;

    else if( b_isinputid && root->id( INPUTITEM_ID ) == i_id )
        return root;

    QList<AbstractPLItem *>::iterator it = root->children.begin();
    while ( it != root->children.end() )
    {
        PLItem *item = static_cast<PLItem *>(*it);
        if( !b_isinputid && item->id( PLAYLIST_ID ) == i_id )
            return item;

        else if( b_isinputid && item->id( INPUTITEM_ID ) == i_id )
            return item;

        if( item->childCount() )
        {
            PLItem *childFound = findInner( item, i_id, b_isinputid );
            if( childFound )
                return childFound;
        }
        ++it;
    }
    return NULL;
}

PLModel::pl_nodetype PLModel::getPLRootType() const
{
    /* can't rely on rootitem as it depends on view / rebuild() */
    AbstractPLItem *plitem = rootItem;
    while( plitem->parent() ) plitem = plitem->parent();

    const input_item_t *p_item = plitem->inputItem();
    if( p_item == p_playlist->p_playing->p_input )
        return ROOTTYPE_CURRENT_PLAYING;

    if( p_playlist->p_media_library &&
        p_item == p_playlist->p_media_library->p_input )
        return ROOTTYPE_MEDIA_LIBRARY;

    return ROOTTYPE_OTHER;
}

bool PLModel::canEdit() const
{
    return ( getPLRootType() != ROOTTYPE_OTHER );
}

/************************* Updates handling *****************************/

/**** Events processing ****/
void PLModel::processInputItemUpdate( )
{
    input_thread_t *p_input = THEMIM->getInput();
    if( !p_input ) return;

    PLItem *item = findByInputId( rootItem, input_GetItem( p_input )->i_id );
    if( item ) emit currentIndexChanged( index( item, 0 ) );

    processInputItemUpdate( input_GetItem( p_input ) );
}

void PLModel::processInputItemUpdate( input_item_t *p_item )
{
    if( !p_item ||  p_item->i_id <= 0 ) return;
    PLItem *item = findByInputId( rootItem, p_item->i_id );
    if( item )
        updateTreeItem( item );
}

void PLModel::processItemRemoval( int i_pl_itemid )
{
    if( i_pl_itemid <= 0 ) return;
    removeItem( findByPLId( rootItem, i_pl_itemid ) );
}

void PLModel::processItemAppend( int i_pl_itemid, int i_pl_itemidparent )
{
    playlist_item_t *p_item = NULL;
    PLItem *newItem = NULL;
    int pos;

    /* Find the Parent */
    PLItem *nodeParentItem = findByPLId( rootItem, i_pl_itemidparent );
    if( !nodeParentItem ) return;

    /* Search for an already matching children */
    foreach( AbstractPLItem *existing, nodeParentItem->children )
        if( existing->id( PLAYLIST_ID ) == i_pl_itemid ) return;

    /* Find the child */
    PL_LOCK;
    p_item = playlist_ItemGetById( p_playlist, i_pl_itemid );
    if( !p_item || p_item->i_flags & PLAYLIST_DBL_FLAG )
    {
        PL_UNLOCK; return;
    }

    for( pos = p_item->p_parent->i_children - 1; pos >= 0; pos-- )
        if( p_item->p_parent->pp_children[pos] == p_item ) break;

    newItem = new PLItem( p_item, nodeParentItem );
    PL_UNLOCK;

    /* We insert the newItem (children) inside the parent */
    beginInsertRows( index( nodeParentItem, 0 ), pos, pos );
    nodeParentItem->insertChild( newItem, pos );
    endInsertRows();
    if ( newItem->inputItem() == THEMIM->currentInputItem() )
        emit currentIndexChanged( index( newItem, 0 ) );

    if( latestSearch.isEmpty() ) return;
    filter( latestSearch, index( rootItem, 0), false /*FIXME*/ );
}

void PLModel::rebuild( playlist_item_t *p_root )
{
    beginResetModel();

    PL_LOCK;
    if( rootItem ) rootItem->clearChildren();
    if( p_root ) // Can be NULL
    {
        if ( rootItem ) delete rootItem;
        rootItem = new PLItem( p_root );
    }
    assert( rootItem );
    /* Recreate from root */
    updateChildren( rootItem );
    PL_UNLOCK;

    /* And signal the view */
    endResetModel();
    if( p_root ) emit rootIndexChanged();
}

void PLModel::takeItem( PLItem *item )
{
    assert( item );
    PLItem *parent = static_cast<PLItem*>(item->parent());
    assert( parent );
    int i_index = parent->indexOf( item );

    beginRemoveRows( index( parent, 0 ), i_index, i_index );
    parent->takeChildAt( i_index );
    endRemoveRows();
}

void PLModel::insertChildren( PLItem *node, QList<PLItem*>& items, int i_pos )
{
    assert( node );
    int count = items.count();
    if( !count ) return;
    beginInsertRows( index( node, 0 ), i_pos, i_pos + count - 1 );
    for( int i = 0; i < count; i++ )
    {
        node->children.insert( i_pos + i, items[i] );
        items[i]->parentItem = node;
    }
    endInsertRows();
}

void PLModel::removeItem( PLItem *item )
{
    if( !item ) return;

    if( item->parent() ) {
        int i = item->parent()->indexOf( item );
        beginRemoveRows( index( static_cast<PLItem*>(item->parent()), 0), i, i );
        item->parent()->children.removeAt(i);
        delete item;
        endRemoveRows();
    }
    else delete item;

    if(item == rootItem)
    {
        rootItem = NULL;
        rebuild( p_playlist->p_playing );
    }
}

/* This function must be entered WITH the playlist lock */
void PLModel::updateChildren( PLItem *root )
{
    playlist_item_t *p_node = playlist_ItemGetById( p_playlist, root->id( PLAYLIST_ID ) );
    updateChildren( p_node, root );
}

/* This function must be entered WITH the playlist lock */
void PLModel::updateChildren( playlist_item_t *p_node, PLItem *root )
{
    for( int i = 0; i < p_node->i_children ; i++ )
    {
        if( p_node->pp_children[i]->i_flags & PLAYLIST_DBL_FLAG ) continue;
        PLItem *newItem =  new PLItem( p_node->pp_children[i], root );
        root->appendChild( newItem );
        if( p_node->pp_children[i]->i_children != -1 )
            updateChildren( p_node->pp_children[i], newItem );
    }
}

/* Function doesn't need playlist-lock, as we don't touch playlist_item_t stuff here*/
void PLModel::updateTreeItem( PLItem *item )
{
    if( !item ) return;
    emit dataChanged( index( item, 0 ) , index( item, columnCount( QModelIndex() ) - 1 ) );
}

/************************* Actions ******************************/

/**
 * Deletion, don't delete items childrens if item is going to be
 * delete allready, so we remove childrens from selection-list.
 */
void PLModel::doDelete( QModelIndexList selected )
{
    if( !canEdit() ) return;

    while( !selected.isEmpty() )
    {
        QModelIndex index = selected[0];
        selected.removeAt( 0 );

        if( index.column() != 0 ) continue;

        PLItem *item = getItem( index );
        if( item->childCount() )
            recurseDelete( item->children, &selected );

        PL_LOCK;
        int i_ret = playlist_DeleteFromInput( p_playlist, item->inputItem(), pl_Locked );
        PL_UNLOCK;

        if( i_ret == VLC_SUCCESS )
            removeItem( item );
    }
}

void PLModel::recurseDelete( QList<AbstractPLItem*> children, QModelIndexList *fullList )
{
    for( int i = children.count() - 1; i >= 0 ; i-- )
    {
        PLItem *item = static_cast<PLItem *>(children[i]);
        if( item->childCount() )
            recurseDelete( item->children, fullList );
        fullList->removeAll( index( item, 0 ) );
    }
}

/******* Volume III: Sorting and searching ********/
void PLModel::sort( const int column, Qt::SortOrder order )
{
    sort( QModelIndex(), indexByPLID( rootItem->id( PLAYLIST_ID ), 0 ) , column, order );
}

void PLModel::sort( QModelIndex caller, QModelIndex rootIndex, const int column, Qt::SortOrder order )
{
    msg_Dbg( p_intf, "Sorting by column %i, order %i", column, order );

    int meta = columnToMeta( column );
    if( meta == COLUMN_END || meta == COLUMN_COVER ) return;

    PLItem *item = ( rootIndex.isValid() ) ? getItem( rootIndex )
                                           : rootItem;
    if( !item ) return;

    int i_root_id = item->id( PLAYLIST_ID );

    QModelIndex qIndex = index( item, 0 );
    int count = item->childCount();
    if( count )
    {
        beginRemoveRows( qIndex, 0, count - 1 );
        item->clearChildren();
        endRemoveRows( );
    }

    PL_LOCK;
    {
        playlist_item_t *p_root = playlist_ItemGetById( p_playlist,
                                                        i_root_id );
        if( p_root )
        {
            playlist_RecursiveNodeSort( p_playlist, p_root,
                                        i_column_sorting( meta ),
                                        order == Qt::AscendingOrder ?
                                            ORDER_NORMAL : ORDER_REVERSE );
        }
    }

    if( count )
    {
        beginInsertRows( qIndex, 0, count - 1 );
        updateChildren( item );
        endInsertRows( );
    }
    PL_UNLOCK;
    /* if we have popup item, try to make sure that you keep that item visible */
    if( caller.isValid() ) emit currentIndexChanged( caller );

    else if( currentIndex().isValid() ) emit currentIndexChanged( currentIndex() );
}

void PLModel::filter( const QString& search_text, const QModelIndex & idx, bool b_recursive )
{
    latestSearch = search_text;

    /** \todo Fire the search with a small delay ? */
    PL_LOCK;
    {
        playlist_item_t *p_root = playlist_ItemGetById( p_playlist,
                                            itemId( idx, PLAYLIST_ID ) );
        assert( p_root );
        playlist_LiveSearchUpdate( p_playlist, p_root, qtu( search_text ),
                                   b_recursive );
        if( idx.isValid() )
        {
            PLItem *searchRoot = getItem( idx );

            beginRemoveRows( idx, 0, searchRoot->childCount() - 1 );
            searchRoot->clearChildren();
            endRemoveRows();

            beginInsertRows( idx, 0, searchRoot->childCount() - 1 );
            updateChildren( searchRoot ); // The PL_LOCK is needed here
            endInsertRows();

            PL_UNLOCK;
            return;
        }
    }
    PL_UNLOCK;
    rebuild();
}

void PLModel::removeAll()
{
    if( rowCount() < 1 ) return;

    QModelIndexList l;
    for( int i = 0; i < rowCount(); i++)
    {
        QModelIndex indexrecord = index( i, 0, QModelIndex() );
        l.append( indexrecord );
    }
    doDelete(l);
}

void PLModel::createNode( QModelIndex index, QString name )
{
    if( name.isEmpty() || !index.isValid() ) return;

    PL_LOCK;
    index = index.parent();
    if ( !index.isValid() ) index = rootIndex();
    playlist_item_t *p_item = playlist_ItemGetById( p_playlist, itemId( index, PLAYLIST_ID ) );
    if( p_item )
        playlist_NodeCreate( p_playlist, qtu( name ), p_item, PLAYLIST_END, 0, NULL );
    PL_UNLOCK;
}

void PLModel::renameNode( QModelIndex index, QString name )
{
    if( name.isEmpty() || !index.isValid() ) return;

    PL_LOCK;
    if ( !index.isValid() ) index = rootIndex();
    input_item_t* p_input = this->getInputItem( index );
    input_item_SetName( p_input, qtu( name ) );
    playlist_t *p_playlist = THEPL;
    input_item_WriteMeta( VLC_OBJECT(p_playlist), p_input );
    PL_UNLOCK;
}

bool PLModel::action( QAction *action, const QModelIndexList &indexes )
{
    QModelIndex index;
    actionsContainerType a = action->data().value<actionsContainerType>();

    switch ( a.action )
    {

    case ACTION_PLAY:
        if ( !indexes.empty() && indexes.first().isValid() )
        {
            activateItem( indexes.first() );
            return true;
        }
        break;

    case ACTION_ADDTOPLAYLIST:
        PL_LOCK;
        foreach( const QModelIndex &currentIndex, indexes )
        {
            playlist_item_t *p_item = playlist_ItemGetById( THEPL, itemId( currentIndex, PLAYLIST_ID ) );
            if( !p_item ) continue;

            playlist_NodeAddCopy( THEPL, p_item,
                                  THEPL->p_playing,
                                  PLAYLIST_END );
        }
        PL_UNLOCK;
        return true;

    case ACTION_REMOVE:
        doDelete( indexes );
        return true;

    case ACTION_SORT:
        if ( indexes.empty() ) break;
        index = indexes.first().parent();
        if( !index.isValid() ) index = rootIndex();
        sort( indexes.first(), index,
              a.column > 0 ? a.column - 1 : -a.column - 1,
              a.column > 0 ? Qt::AscendingOrder : Qt::DescendingOrder );
        return true;

    case ACTION_CLEAR:
        removeAll();
        return true;

    case ACTION_ENQUEUEFILE:
        foreach( const QString &uri, a.uris )
            Open::openMRL( p_intf, uri.toLatin1().constData(),
                           false, getPLRootType() == ROOTTYPE_CURRENT_PLAYING );
        return true;

    case ACTION_ENQUEUEDIR:
        if( a.uris.isEmpty() ) break;

        Open::openMRL( p_intf, a.uris.first().toLatin1().constData(),
                       false, getPLRootType() == ROOTTYPE_CURRENT_PLAYING );

        return true;

    case ACTION_ENQUEUEGENERIC:
        foreach( const QString &uri, a.uris )
        {
            QStringList options = a.options.split( " :" );
            Open::openMRLwithOptions( p_intf, uri, &options, false );
        }
        return true;

    default:
        break;
    }
    return false;
}

bool PLModel::isSupportedAction( actions action, const QModelIndex &index ) const
{
    if( !index.isValid() )
        return false;

    const PLItem *item = getItem( index );

    switch ( action )
    {
    case ACTION_ADDTOPLAYLIST:
        /* Only if we are not already in Current Playing */
        if ( getPLRootType() == ROOTTYPE_CURRENT_PLAYING ) return false;
        if( index != rootIndex() )
            return ( item->id( PLAYLIST_ID ) != THEPL->p_playing->i_id );
    case ACTION_SORT:
        return rowCount() && !item->readOnly();
    case ACTION_PLAY:
    case ACTION_STREAM:
    case ACTION_SAVE:
    case ACTION_INFO:
        return index != rootIndex();
    case ACTION_REMOVE:
        return index != rootIndex() && !item->readOnly();
    case ACTION_EXPLORE:
            return getURI( index ).startsWith( "file://" );
    case ACTION_CREATENODE:
            return ( isTree() && !item->readOnly() );
    case ACTION_RENAMENODE:
            return ( index != rootIndex() ) && !isLeaf( index ) && !item->readOnly();
    case ACTION_CLEAR:
            return rowCount() && !item->readOnly();
    case ACTION_ENQUEUEFILE:
    case ACTION_ENQUEUEDIR:
    case ACTION_ENQUEUEGENERIC:
        return !item->readOnly();
    case ACTION_SAVETOPLAYLIST:
        return rowCount() > 0;
    default:
        return false;
    }
    return false;
}

/******************* Drag and Drop helper class ******************/
PlMimeData::~PlMimeData()
{
    foreach( input_item_t *p_item, _inputItems )
        vlc_gc_decref( p_item );
}

void PlMimeData::appendItem( input_item_t *p_item )
{
    vlc_gc_incref( p_item );
    _inputItems.append( p_item );
}

QList<input_item_t*> PlMimeData::inputItems() const
{
    return _inputItems;
}

QStringList PlMimeData::formats () const
{
    QStringList fmts;
    fmts << "vlc/qt-input-items";
    return fmts;
}
