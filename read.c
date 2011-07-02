/*****************************************************************************
 * read.c:
 *****************************************************************************
 * Copyright (C) 2010 L-SMASH project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/

/* This file is available under an ISC license. */

#ifdef LSMASH_DEMUXER_ENABLED

#include "internal.h" /* must be placed first */

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "box.h"
#include "print.h"


static int isom_read_box( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, uint64_t parent_pos, int level );

static int isom_bs_read_box_common( lsmash_bs_t *bs, isom_box_t *box )
{
    box->pos = lsmash_ftell( bs->stream );
    /* read size and type */
    if( lsmash_bs_read_data( bs, ISOM_DEFAULT_BOX_HEADER_SIZE ) )
        return -1;
    if( feof( bs->stream ) )
        return 1;
    box->size = lsmash_bs_get_be32( bs );
    box->type = lsmash_bs_get_be32( bs );
    if( box->size == 1 )
    {
        if( lsmash_bs_read_data( bs, sizeof(uint64_t) ) )
            return -1;
        box->size = lsmash_bs_get_be64( bs );
    }
    if( !box->size )
        box->size = UINT64_MAX;
    /* read version and flags */
    if( isom_is_fullbox( box ) )
    {
        if( lsmash_bs_read_data( bs, sizeof(uint32_t) ) )
            return -1;
        box->version = lsmash_bs_get_byte( bs );
        box->flags   = lsmash_bs_get_be24( bs );
    }
    return 0;
}

static void isom_basebox_common_copy( isom_box_t *dst, isom_box_t *src )
{
    dst->root     = src->root;
    dst->parent   = src->parent;
    dst->manager  = src->manager;
    dst->pos      = src->pos;
    dst->size     = src->size;
    dst->type     = src->type;
    dst->usertype = src->usertype;
}

static void isom_fullbox_common_copy( isom_box_t *dst, isom_box_t *src )
{
    dst->root     = src->root;
    dst->parent   = src->parent;
    dst->manager  = src->manager;
    dst->pos      = src->pos;
    dst->size     = src->size;
    dst->type     = src->type;
    dst->usertype = src->usertype;
    dst->version  = src->version;
    dst->flags    = src->flags;
}

static void isom_box_common_copy( void *dst, void *src )
{
    if( src && ((isom_box_t *)src)->type == ISOM_BOX_TYPE_STSD )
    {
        isom_basebox_common_copy( (isom_box_t *)dst, (isom_box_t *)src );
        return;
    }
    if( isom_is_fullbox( src ) )
        isom_fullbox_common_copy( (isom_box_t *)dst, (isom_box_t *)src );
    else
        isom_basebox_common_copy( (isom_box_t *)dst, (isom_box_t *)src );
}

static void isom_read_box_rest( lsmash_bs_t *bs, isom_box_t *box )
{
    if( lsmash_bs_read_data( bs, box->size - lsmash_bs_get_pos( bs ) ) )
        return;
    if( box->size != bs->store )  /* not match size */
        bs->error = 1;
}

static void isom_skip_box_rest( lsmash_bs_t *bs, isom_box_t *box )
{
    uint64_t skip_bytes = box->size - lsmash_bs_get_pos( bs );
    if( bs->stream != stdin )
        lsmash_fseek( bs->stream, skip_bytes, SEEK_CUR );
    else
        for( uint64_t i = 0; i < skip_bytes; i++ )
            if( fgetc( stdin ) == EOF )
                break;
}

static int isom_read_children( lsmash_root_t *root, isom_box_t *box, void *parent, int level )
{
    int ret;
    lsmash_bs_t *bs = root->bs;
    isom_box_t *parent_box = (isom_box_t *)parent;
    uint64_t parent_pos = lsmash_bs_get_pos( bs );
    while( !(ret = isom_read_box( root, box, parent_box, parent_pos, level )) )
    {
        parent_pos += box->size;
        if( parent_box->size <= parent_pos || bs->error )
            break;
    }
    box->size = parent_pos;    /* for ROOT size */
    return ret;
}

static int isom_read_unknown_box( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    isom_box_t *unknown = malloc( sizeof(isom_box_t) );
    if( !unknown )
        return -1;
    memset( unknown, 0, sizeof(isom_box_t) );
    lsmash_bs_t *bs = root->bs;
    isom_skip_box_rest( bs, box );
    unknown->parent = parent;
    unknown->size = box->size;
    unknown->type = box->type;
    unknown->manager = 0x03;   /* add unknown box flag + free flag */
    if( isom_add_print_func( root, unknown, level ) )
    {
        free( unknown );
        return -1;
    }
    return 0;
}

static int isom_read_ftyp( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( !!parent->type )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( ftyp, parent, box->type );
    ((lsmash_root_t *)parent)->ftyp = ftyp;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    ftyp->major_brand              = lsmash_bs_get_be32( bs );
    ftyp->minor_version            = lsmash_bs_get_be32( bs );
    uint64_t pos = lsmash_bs_get_pos( bs );
    ftyp->brand_count = box->size > pos ? (box->size - pos) / sizeof(uint32_t) : 0;
    ftyp->compatible_brands = ftyp->brand_count ? malloc( ftyp->brand_count * sizeof(uint32_t) ) : NULL;
    if( !ftyp->compatible_brands )
        return -1;
    for( uint32_t i = 0; i < ftyp->brand_count; i++ )
        ftyp->compatible_brands[i] = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( ftyp, box );
    return isom_add_print_func( root, ftyp, level );
}

static int isom_read_moov( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( !!parent->type )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( moov, parent, box->type );
    ((lsmash_root_t *)parent)->moov = moov;
    isom_box_common_copy( moov, box );
    if( isom_add_print_func( root, moov, level ) )
        return -1;
    return isom_read_children( root, box, moov, level );
}

static int isom_read_mvhd( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MOOV )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( mvhd, parent, box->type );
    ((isom_moov_t *)parent)->mvhd = mvhd;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    if( box->version )
    {
        mvhd->creation_time     = lsmash_bs_get_be64( bs );
        mvhd->modification_time = lsmash_bs_get_be64( bs );
        mvhd->timescale         = lsmash_bs_get_be32( bs );
        mvhd->duration          = lsmash_bs_get_be64( bs );
    }
    else
    {
        mvhd->creation_time     = lsmash_bs_get_be32( bs );
        mvhd->modification_time = lsmash_bs_get_be32( bs );
        mvhd->timescale         = lsmash_bs_get_be32( bs );
        mvhd->duration          = lsmash_bs_get_be32( bs );
    }
    mvhd->rate              = lsmash_bs_get_be32( bs );
    mvhd->volume            = lsmash_bs_get_be16( bs );
    mvhd->reserved          = lsmash_bs_get_be16( bs );
    mvhd->preferredLong[0]  = lsmash_bs_get_be32( bs );
    mvhd->preferredLong[1]  = lsmash_bs_get_be32( bs );
    for( int i = 0; i < 9; i++ )
        mvhd->matrix[i]     = lsmash_bs_get_be32( bs );
    mvhd->previewTime       = lsmash_bs_get_be32( bs );
    mvhd->previewDuration   = lsmash_bs_get_be32( bs );
    mvhd->posterTime        = lsmash_bs_get_be32( bs );
    mvhd->selectionTime     = lsmash_bs_get_be32( bs );
    mvhd->selectionDuration = lsmash_bs_get_be32( bs );
    mvhd->currentTime       = lsmash_bs_get_be32( bs );
    mvhd->next_track_ID     = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( mvhd, box );
    return isom_add_print_func( root, mvhd, level );
}

static int isom_read_iods( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MOOV )
        return isom_read_unknown_box( root, box, parent, level );
    isom_box_t *iods = malloc( sizeof(isom_box_t) );
    if( !iods )
        return -1;
    memset( iods, 0, sizeof(isom_box_t) );
    lsmash_bs_t *bs = root->bs;
    isom_skip_box_rest( bs, box );
    box->manager |= 0x02;       /* add free flag */
    isom_box_common_copy( iods, box );
    if( isom_add_print_func( root, iods, level ) )
    {
        free( iods );
        return -1;
    }
    return 0;
}

static int isom_read_esds( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    isom_box_t *esds = malloc( sizeof(isom_box_t) );
    if( !esds )
        return -1;
    memset( esds, 0, sizeof(isom_box_t) );
    lsmash_bs_t *bs = root->bs;
    isom_skip_box_rest( bs, box );
    box->manager |= 0x02;       /* add free flag */
    isom_box_common_copy( esds, box );
    if( isom_add_print_func( root, esds, level ) )
    {
        free( esds );
        return -1;
    }
    return 0;
}

static int isom_read_trak( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MOOV )
        return isom_read_unknown_box( root, box, parent, level );
    lsmash_entry_list_t *list = ((isom_moov_t *)parent)->trak_list;
    if( !list )
    {
        list = lsmash_create_entry_list();
        if( !list )
            return -1;
        ((isom_moov_t *)parent)->trak_list = list;
    }
    isom_trak_entry_t *trak = malloc( sizeof(isom_trak_entry_t) );
    if( !trak )
        return -1;
    memset( trak, 0, sizeof(isom_trak_entry_t) );
    isom_cache_t *cache = malloc( sizeof(isom_cache_t) );
    if( !cache )
    {
        free( trak );
        return -1;
    }
    memset( cache, 0, sizeof(isom_cache_t) );
    trak->root = root;
    trak->cache = cache;
    if( lsmash_add_entry( list, trak ) )
    {
        free( trak->cache );
        free( trak );
        return -1;
    }
    box->parent = parent;
    isom_box_common_copy( trak, box );
    if( isom_add_print_func( root, trak, level ) )
        return -1;
    return isom_read_children( root, box, trak, level );
}

static int isom_read_tkhd( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_TRAK )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( tkhd, parent, box->type );
    ((isom_trak_entry_t *)parent)->tkhd = tkhd;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    if( box->version )
    {
        tkhd->creation_time     = lsmash_bs_get_be64( bs );
        tkhd->modification_time = lsmash_bs_get_be64( bs );
        tkhd->track_ID          = lsmash_bs_get_be32( bs );
        tkhd->reserved1         = lsmash_bs_get_be32( bs );
        tkhd->duration          = lsmash_bs_get_be64( bs );
    }
    else
    {
        tkhd->creation_time     = lsmash_bs_get_be32( bs );
        tkhd->modification_time = lsmash_bs_get_be32( bs );
        tkhd->track_ID          = lsmash_bs_get_be32( bs );
        tkhd->reserved1         = lsmash_bs_get_be32( bs );
        tkhd->duration          = lsmash_bs_get_be32( bs );
    }
    tkhd->reserved2[0]    = lsmash_bs_get_be32( bs );
    tkhd->reserved2[1]    = lsmash_bs_get_be32( bs );
    tkhd->layer           = lsmash_bs_get_be16( bs );
    tkhd->alternate_group = lsmash_bs_get_be16( bs );
    tkhd->volume          = lsmash_bs_get_be16( bs );
    tkhd->reserved3       = lsmash_bs_get_be16( bs );
    for( int i = 0; i < 9; i++ )
        tkhd->matrix[i]   = lsmash_bs_get_be32( bs );
    tkhd->width           = lsmash_bs_get_be32( bs );
    tkhd->height          = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( tkhd, box );
    return isom_add_print_func( root, tkhd, level );
}

static int isom_read_tapt( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_TRAK )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( tapt, parent, box->type );
    ((isom_trak_entry_t *)parent)->tapt = tapt;
    isom_box_common_copy( tapt, box );
    if( isom_add_print_func( root, tapt, level ) )
        return -1;
    return isom_read_children( root, box, tapt, level );
}

static int isom_read_clef( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != QT_BOX_TYPE_TAPT )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( clef, parent, box->type );
    ((isom_tapt_t *)parent)->clef = clef;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    clef->width  = lsmash_bs_get_be32( bs );
    clef->height = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( clef, box );
    return isom_add_print_func( root, clef, level );
}

static int isom_read_prof( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != QT_BOX_TYPE_TAPT )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( prof, parent, box->type );
    ((isom_tapt_t *)parent)->prof = prof;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    prof->width  = lsmash_bs_get_be32( bs );
    prof->height = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( prof, box );
    return isom_add_print_func( root, prof, level );
}

static int isom_read_enof( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != QT_BOX_TYPE_TAPT )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( enof, parent, box->type );
    ((isom_tapt_t *)parent)->enof = enof;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    enof->width  = lsmash_bs_get_be32( bs );
    enof->height = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( enof, box );
    return isom_add_print_func( root, enof, level );
}

static int isom_read_edts( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_TRAK )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( edts, parent, box->type );
    ((isom_trak_entry_t *)parent)->edts = edts;
    isom_box_common_copy( edts, box );
    if( isom_add_print_func( root, edts, level ) )
        return -1;
    return isom_read_children( root, box, edts, level );
}

static int isom_read_elst( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_EDTS )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_list_box( elst, parent, box->type );
    ((isom_edts_t *)parent)->elst = elst;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    uint32_t entry_count = lsmash_bs_get_be32( bs );
    uint64_t pos;
    for( pos = lsmash_bs_get_pos( bs ); pos < box->size; pos = lsmash_bs_get_pos( bs ) )
    {
        isom_elst_entry_t *data = malloc( sizeof(isom_elst_entry_t) );
        if( !data || lsmash_add_entry( elst->list, data ) )
        {
            if( data )
                free( data );
            return -1;
        }
        memset( data, 0, sizeof(isom_elst_entry_t) );
        if( box->version == 1 )
        {
            data->segment_duration =          lsmash_bs_get_be64( bs );
            data->media_time       = (int64_t)lsmash_bs_get_be64( bs );
        }
        else
        {
            data->segment_duration =          lsmash_bs_get_be32( bs );
            data->media_time       = (int32_t)lsmash_bs_get_be32( bs );
        }
        data->media_rate = lsmash_bs_get_be32( bs );
    }
    if( entry_count != elst->list->entry_count || box->size < pos )
        printf( "[elst] box has extra bytes: %"PRId64"\n", pos - box->size );
    box->size = pos;
    isom_box_common_copy( elst, box );
    return isom_add_print_func( root, elst, level );
}

static int isom_read_tref( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_TRAK )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( tref, parent, box->type );
    ((isom_trak_entry_t *)parent)->tref = tref;
    isom_box_common_copy( tref, box );
    if( isom_add_print_func( root, tref, level ) )
        return -1;
    return isom_read_children( root, box, tref, level );
}

static int isom_read_track_reference_type( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_TREF )
        return isom_read_unknown_box( root, box, parent, level );
    isom_tref_t *tref = (isom_tref_t *)parent;
    lsmash_entry_list_t *list = tref->ref_list;
    if( !list )
    {
        list = lsmash_create_entry_list();
        if( !list )
            return -1;
        tref->ref_list = list;
    }
    isom_tref_type_t *ref = malloc( sizeof(isom_tref_type_t) );
    if( !ref )
        return -1;
    memset( ref, 0, sizeof(isom_tref_type_t) );
    if( lsmash_add_entry( list, ref ) )
    {
        free( ref );
        return -1;
    }
    lsmash_bs_t *bs = root->bs;
    ref->ref_count = (box->size - lsmash_bs_get_pos( bs ) ) / sizeof(uint32_t);
    if( ref->ref_count )
    {
        ref->track_ID = malloc( ref->ref_count * sizeof(uint32_t) );
        if( !ref->track_ID )
        {
            ref->ref_count = 0;
            return -1;
        }
        isom_read_box_rest( bs, box );
        for( uint32_t i = 0; i < ref->ref_count; i++ )
            ref->track_ID[i] = lsmash_bs_get_be32( bs );
    }
    uint64_t pos = lsmash_bs_get_pos( bs );
    if( box->size != pos )
        printf( "[%s] box has extra bytes: %"PRId64"\n", isom_4cc2str( box->type ), box->size - pos );
    box->size = pos;
    isom_box_common_copy( ref, box );
    return isom_add_print_func( root, ref, level );
}

static int isom_read_mdia( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_TRAK )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( mdia, parent, box->type );
    ((isom_trak_entry_t *)parent)->mdia = mdia;
    isom_box_common_copy( mdia, box );
    if( isom_add_print_func( root, mdia, level ) )
        return -1;
    return isom_read_children( root, box, mdia, level );
}

static int isom_read_mdhd( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MDIA )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( mdhd, parent, box->type );
    ((isom_mdia_t *)parent)->mdhd = mdhd;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    if( box->version )
    {
        mdhd->creation_time     = lsmash_bs_get_be64( bs );
        mdhd->modification_time = lsmash_bs_get_be64( bs );
        mdhd->timescale         = lsmash_bs_get_be32( bs );
        mdhd->duration          = lsmash_bs_get_be64( bs );
    }
    else
    {
        mdhd->creation_time     = lsmash_bs_get_be32( bs );
        mdhd->modification_time = lsmash_bs_get_be32( bs );
        mdhd->timescale         = lsmash_bs_get_be32( bs );
        mdhd->duration          = lsmash_bs_get_be32( bs );
    }
    mdhd->language = lsmash_bs_get_be16( bs );
    mdhd->quality  = lsmash_bs_get_be16( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( mdhd, box );
    return isom_add_print_func( root, mdhd, level );
}

static int isom_read_hdlr( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MDIA
     //&& parent->type != ISOM_BOX_TYPE_META
     && parent->type != ISOM_BOX_TYPE_MINF )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( hdlr, parent, box->type );
    if( parent->type == ISOM_BOX_TYPE_MDIA )
        ((isom_mdia_t *)parent)->hdlr = hdlr;
    else
        ((isom_minf_t *)parent)->hdlr = hdlr;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    hdlr->componentType         = lsmash_bs_get_be32( bs );
    hdlr->componentSubtype      = lsmash_bs_get_be32( bs );
    hdlr->componentManufacturer = lsmash_bs_get_be32( bs );
    hdlr->componentFlags        = lsmash_bs_get_be32( bs );
    hdlr->componentFlagsMask    = lsmash_bs_get_be32( bs );
    uint64_t pos = lsmash_bs_get_pos( bs );
    hdlr->componentName_length = box->size - pos;
    if( hdlr->componentName_length )
    {
        hdlr->componentName = malloc( hdlr->componentName_length );
        if( !hdlr->componentName )
            return -1;
        for( uint32_t i = 0; pos < box->size; pos = lsmash_bs_get_pos( bs ) )
            hdlr->componentName[i++] = lsmash_bs_get_byte( bs );
    }
    box->size = pos;
    isom_box_common_copy( hdlr, box );
    return isom_add_print_func( root, hdlr, level );
}

static int isom_read_minf( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MDIA )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( minf, parent, box->type );
    ((isom_mdia_t *)parent)->minf = minf;
    isom_box_common_copy( minf, box );
    if( isom_add_print_func( root, minf, level ) )
        return -1;
    return isom_read_children( root, box, minf, level );
}

static int isom_read_vmhd( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MINF )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( vmhd, parent, box->type );
    ((isom_minf_t *)parent)->vmhd = vmhd;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    vmhd->graphicsmode   = lsmash_bs_get_be16( bs );
    for( int i = 0; i < 3; i++ )
        vmhd->opcolor[i] = lsmash_bs_get_be16( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( vmhd, box );
    return isom_add_print_func( root, vmhd, level );
}

static int isom_read_smhd( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MINF )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( smhd, parent, box->type );
    ((isom_minf_t *)parent)->smhd = smhd;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    smhd->balance  = lsmash_bs_get_be16( bs );
    smhd->reserved = lsmash_bs_get_be16( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( smhd, box );
    return isom_add_print_func( root, smhd, level );
}

static int isom_read_hmhd( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MINF )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( hmhd, parent, box->type );
    ((isom_minf_t *)parent)->hmhd = hmhd;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    hmhd->maxPDUsize = lsmash_bs_get_be16( bs );
    hmhd->avgPDUsize = lsmash_bs_get_be16( bs );
    hmhd->maxbitrate = lsmash_bs_get_be32( bs );
    hmhd->avgbitrate = lsmash_bs_get_be32( bs );
    hmhd->reserved   = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( hmhd, box );
    return isom_add_print_func( root, hmhd, level );
}

static int isom_read_nmhd( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MINF )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( nmhd, parent, box->type );
    ((isom_minf_t *)parent)->nmhd = nmhd;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( nmhd, box );
    return isom_add_print_func( root, nmhd, level );
}

static int isom_read_gmhd( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MINF )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( gmhd, parent, box->type );
    ((isom_minf_t *)parent)->gmhd = gmhd;
    isom_box_common_copy( gmhd, box );
    if( isom_add_print_func( root, gmhd, level ) )
        return -1;
    return isom_read_children( root, box, gmhd, level );
}

static int isom_read_gmin( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != QT_BOX_TYPE_GMHD )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( gmin, parent, box->type );
    ((isom_gmhd_t *)parent)->gmin = gmin;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    gmin->graphicsmode   = lsmash_bs_get_be16( bs );
    for( int i = 0; i < 3; i++ )
        gmin->opcolor[i] = lsmash_bs_get_be16( bs );
    gmin->balance        = lsmash_bs_get_be16( bs );
    gmin->reserved       = lsmash_bs_get_be16( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( gmin, box );
    return isom_add_print_func( root, gmin, level );
}

static int isom_read_text( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != QT_BOX_TYPE_GMHD )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( text, parent, box->type );
    ((isom_gmhd_t *)parent)->text = text;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    for( int i = 0; i < 9; i++ )
        text->matrix[i] = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( text, box );
    return isom_add_print_func( root, text, level );
}

static int isom_read_dinf( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MINF )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( dinf, parent, box->type );
    ((isom_minf_t *)parent)->dinf = dinf;
    isom_box_common_copy( dinf, box );
    if( isom_add_print_func( root, dinf, level ) )
        return -1;
    return isom_read_children( root, box, dinf, level );
}

static int isom_read_dref( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_DINF /*&& parent->type != ISOM_BOX_TYPE_META*/ )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_list_box( dref, parent, box->type );
    ((isom_dinf_t *)parent)->dref = dref;
    lsmash_bs_t *bs = root->bs;
    if( lsmash_bs_read_data( bs, sizeof(uint32_t) ) )
        return -1;
    dref->list->entry_count = lsmash_bs_get_be32( bs );
    isom_box_common_copy( dref, box );
    if( isom_add_print_func( root, dref, level ) )
        return -1;
    return isom_read_children( root, box, dref, level );
}

static int isom_read_url( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_DREF )
        return isom_read_unknown_box( root, box, parent, level );
    lsmash_entry_list_t *list = ((isom_dref_t *)parent)->list;
    if( !list )
        return -1;
    isom_dref_entry_t *url = malloc( sizeof(isom_dref_entry_t) );
    if( !url )
        return -1;
    memset( url, 0, sizeof(isom_dref_entry_t) );
    if( !list->head )
        list->entry_count = 0;      /* discard entry_count gotten from the file */
    if( lsmash_add_entry( list, url ) )
    {
        free( url );
        return -1;
    }
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    uint64_t pos = lsmash_bs_get_pos( bs );
    url->location_length = box->size - pos;
    if( url->location_length )
    {
        url->location = malloc( url->location_length );
        if( !url->location )
            return -1;
        for( uint32_t i = 0; pos < box->size; pos = lsmash_bs_get_pos( bs ) )
            url->location[i++] = lsmash_bs_get_byte( bs );
    }
    box->size = pos;
    box->parent = parent;
    isom_box_common_copy( url, box );
    return isom_add_print_func( root, url, level );
}

static int isom_read_stbl( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MINF )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( stbl, parent, box->type );
    ((isom_minf_t *)parent)->stbl = stbl;
    isom_box_common_copy( stbl, box );
    if( isom_add_print_func( root, stbl, level ) )
        return -1;
    return isom_read_children( root, box, stbl, level );
}

static int isom_read_stsd( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STBL )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_list_box( stsd, parent, box->type );
    ((isom_stbl_t *)parent)->stsd = stsd;
    lsmash_bs_t *bs = root->bs;
    if( lsmash_bs_read_data( bs, sizeof(uint32_t) ) )
        return -1;
    stsd->list->entry_count = lsmash_bs_get_be32( bs );
    isom_box_common_copy( stsd, box );
    if( isom_add_print_func( root, stsd, level ) )
        return -1;
    return isom_read_children( root, box, stsd, level );
}

static void *isom_sample_description_alloc( uint32_t sample_type )
{
    switch( sample_type )
    {
        case ISOM_CODEC_TYPE_AVC1_VIDEO :
        case ISOM_CODEC_TYPE_AVC2_VIDEO :
        case ISOM_CODEC_TYPE_AVCP_VIDEO :
        case ISOM_CODEC_TYPE_MVC1_VIDEO :
        case ISOM_CODEC_TYPE_MVC2_VIDEO :
        case ISOM_CODEC_TYPE_MP4V_VIDEO :
        case ISOM_CODEC_TYPE_DRAC_VIDEO :
        case ISOM_CODEC_TYPE_ENCV_VIDEO :
        case ISOM_CODEC_TYPE_MJP2_VIDEO :
        case ISOM_CODEC_TYPE_S263_VIDEO :
        case ISOM_CODEC_TYPE_SVC1_VIDEO :
        case ISOM_CODEC_TYPE_VC_1_VIDEO :
            return malloc( sizeof(isom_visual_entry_t) );
        case ISOM_CODEC_TYPE_AC_3_AUDIO :
        case ISOM_CODEC_TYPE_ALAC_AUDIO :
        case ISOM_CODEC_TYPE_DRA1_AUDIO :
        case ISOM_CODEC_TYPE_DTSC_AUDIO :
        case ISOM_CODEC_TYPE_DTSH_AUDIO :
        case ISOM_CODEC_TYPE_DTSL_AUDIO :
        case ISOM_CODEC_TYPE_DTSE_AUDIO :
        case ISOM_CODEC_TYPE_EC_3_AUDIO :
        case ISOM_CODEC_TYPE_ENCA_AUDIO :
        case ISOM_CODEC_TYPE_G719_AUDIO :
        case ISOM_CODEC_TYPE_G726_AUDIO :
        case ISOM_CODEC_TYPE_M4AE_AUDIO :
        case ISOM_CODEC_TYPE_MLPA_AUDIO :
        case ISOM_CODEC_TYPE_MP4A_AUDIO :
        //case ISOM_CODEC_TYPE_RAW_AUDIO  :
        case ISOM_CODEC_TYPE_SAMR_AUDIO :
        case ISOM_CODEC_TYPE_SAWB_AUDIO :
        case ISOM_CODEC_TYPE_SAWP_AUDIO :
        case ISOM_CODEC_TYPE_SEVC_AUDIO :
        case ISOM_CODEC_TYPE_SQCP_AUDIO :
        case ISOM_CODEC_TYPE_SSMV_AUDIO :
        //case ISOM_CODEC_TYPE_TWOS_AUDIO :
        case QT_CODEC_TYPE_23NI_AUDIO :
        case QT_CODEC_TYPE_MAC3_AUDIO :
        case QT_CODEC_TYPE_MAC6_AUDIO :
        case QT_CODEC_TYPE_NONE_AUDIO :
        case QT_CODEC_TYPE_QDM2_AUDIO :
        case QT_CODEC_TYPE_QDMC_AUDIO :
        case QT_CODEC_TYPE_QCLP_AUDIO :
        case QT_CODEC_TYPE_AGSM_AUDIO :
        case QT_CODEC_TYPE_ALAW_AUDIO :
        case QT_CODEC_TYPE_CDX2_AUDIO :
        case QT_CODEC_TYPE_CDX4_AUDIO :
        case QT_CODEC_TYPE_DVCA_AUDIO :
        case QT_CODEC_TYPE_DVI_AUDIO  :
        case QT_CODEC_TYPE_FL32_AUDIO :
        case QT_CODEC_TYPE_FL64_AUDIO :
        case QT_CODEC_TYPE_IMA4_AUDIO :
        case QT_CODEC_TYPE_IN24_AUDIO :
        case QT_CODEC_TYPE_IN32_AUDIO :
        case QT_CODEC_TYPE_LPCM_AUDIO :
        case QT_CODEC_TYPE_RAW_AUDIO :
        case QT_CODEC_TYPE_SOWT_AUDIO :
        case QT_CODEC_TYPE_TWOS_AUDIO :
        case QT_CODEC_TYPE_ULAW_AUDIO :
        case QT_CODEC_TYPE_VDVA_AUDIO :
        case QT_CODEC_TYPE_FULLMP3_AUDIO :
        case QT_CODEC_TYPE_MP3_AUDIO :
        case QT_CODEC_TYPE_ADPCM2_AUDIO :
        case QT_CODEC_TYPE_ADPCM17_AUDIO :
        case QT_CODEC_TYPE_GSM49_AUDIO :
        case QT_CODEC_TYPE_NOT_SPECIFIED :
            return malloc( sizeof(isom_audio_entry_t) );
        case ISOM_CODEC_TYPE_TX3G_TEXT :
            return malloc( sizeof(isom_tx3g_entry_t) );
        case QT_CODEC_TYPE_TEXT_TEXT :
            return malloc( sizeof(isom_text_entry_t) );
        default :
            return NULL;
    }
}

static void isom_sample_description_init( void *sample, uint32_t sample_type )
{
    switch( sample_type )
    {
        case ISOM_CODEC_TYPE_AVC1_VIDEO :
        case ISOM_CODEC_TYPE_AVC2_VIDEO :
        case ISOM_CODEC_TYPE_AVCP_VIDEO :
        case ISOM_CODEC_TYPE_MVC1_VIDEO :
        case ISOM_CODEC_TYPE_MVC2_VIDEO :
        case ISOM_CODEC_TYPE_MP4V_VIDEO :
        case ISOM_CODEC_TYPE_DRAC_VIDEO :
        case ISOM_CODEC_TYPE_ENCV_VIDEO :
        case ISOM_CODEC_TYPE_MJP2_VIDEO :
        case ISOM_CODEC_TYPE_S263_VIDEO :
        case ISOM_CODEC_TYPE_SVC1_VIDEO :
        case ISOM_CODEC_TYPE_VC_1_VIDEO :
            memset( sample, 0, sizeof(isom_visual_entry_t) );
            break;
        case ISOM_CODEC_TYPE_AC_3_AUDIO :
        case ISOM_CODEC_TYPE_ALAC_AUDIO :
        case ISOM_CODEC_TYPE_DRA1_AUDIO :
        case ISOM_CODEC_TYPE_DTSC_AUDIO :
        case ISOM_CODEC_TYPE_DTSH_AUDIO :
        case ISOM_CODEC_TYPE_DTSL_AUDIO :
        case ISOM_CODEC_TYPE_DTSE_AUDIO :
        case ISOM_CODEC_TYPE_EC_3_AUDIO :
        case ISOM_CODEC_TYPE_ENCA_AUDIO :
        case ISOM_CODEC_TYPE_G719_AUDIO :
        case ISOM_CODEC_TYPE_G726_AUDIO :
        case ISOM_CODEC_TYPE_M4AE_AUDIO :
        case ISOM_CODEC_TYPE_MLPA_AUDIO :
        case ISOM_CODEC_TYPE_MP4A_AUDIO :
        //case ISOM_CODEC_TYPE_RAW_AUDIO  :
        case ISOM_CODEC_TYPE_SAMR_AUDIO :
        case ISOM_CODEC_TYPE_SAWB_AUDIO :
        case ISOM_CODEC_TYPE_SAWP_AUDIO :
        case ISOM_CODEC_TYPE_SEVC_AUDIO :
        case ISOM_CODEC_TYPE_SQCP_AUDIO :
        case ISOM_CODEC_TYPE_SSMV_AUDIO :
        //case ISOM_CODEC_TYPE_TWOS_AUDIO :
        case QT_CODEC_TYPE_23NI_AUDIO :
        case QT_CODEC_TYPE_MAC3_AUDIO :
        case QT_CODEC_TYPE_MAC6_AUDIO :
        case QT_CODEC_TYPE_NONE_AUDIO :
        case QT_CODEC_TYPE_QDM2_AUDIO :
        case QT_CODEC_TYPE_QDMC_AUDIO :
        case QT_CODEC_TYPE_QCLP_AUDIO :
        case QT_CODEC_TYPE_AGSM_AUDIO :
        case QT_CODEC_TYPE_ALAW_AUDIO :
        case QT_CODEC_TYPE_CDX2_AUDIO :
        case QT_CODEC_TYPE_CDX4_AUDIO :
        case QT_CODEC_TYPE_DVCA_AUDIO :
        case QT_CODEC_TYPE_DVI_AUDIO  :
        case QT_CODEC_TYPE_FL32_AUDIO :
        case QT_CODEC_TYPE_FL64_AUDIO :
        case QT_CODEC_TYPE_IMA4_AUDIO :
        case QT_CODEC_TYPE_IN24_AUDIO :
        case QT_CODEC_TYPE_IN32_AUDIO :
        case QT_CODEC_TYPE_LPCM_AUDIO :
        case QT_CODEC_TYPE_RAW_AUDIO :
        case QT_CODEC_TYPE_SOWT_AUDIO :
        case QT_CODEC_TYPE_TWOS_AUDIO :
        case QT_CODEC_TYPE_ULAW_AUDIO :
        case QT_CODEC_TYPE_VDVA_AUDIO :
        case QT_CODEC_TYPE_FULLMP3_AUDIO :
        case QT_CODEC_TYPE_MP3_AUDIO :
        case QT_CODEC_TYPE_ADPCM2_AUDIO :
        case QT_CODEC_TYPE_ADPCM17_AUDIO :
        case QT_CODEC_TYPE_GSM49_AUDIO :
        case QT_CODEC_TYPE_NOT_SPECIFIED :
            memset( sample, 0, sizeof(isom_audio_entry_t) );
            break;
        case ISOM_CODEC_TYPE_TX3G_TEXT :
            memset( sample, 0, sizeof(isom_tx3g_entry_t) );
            break;
        case QT_CODEC_TYPE_TEXT_TEXT :
            memset( sample, 0, sizeof(isom_text_entry_t) );
            break;
        default :
            break;
    }
}

static void *isom_add_description( uint32_t sample_type, lsmash_entry_list_t *list )
{
    if( !list )
        return NULL;
    void *sample = isom_sample_description_alloc( sample_type );
    if( !sample )
        return NULL;
    isom_sample_description_init( sample, sample_type );
    if( !list->head )
        list->entry_count = 0;      /* discard entry_count gotten from the file */
    if( lsmash_add_entry( list, sample ) )
    {
        free( sample );
        return NULL;
    }
    return sample;
}

static int isom_read_visual_description( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STSD )
        return isom_read_unknown_box( root, box, parent, level );
    isom_visual_entry_t *visual = (isom_visual_entry_t *)isom_add_description( box->type, ((isom_stsd_t *)parent)->list );
    if( !visual )
        return -1;
    lsmash_bs_t *bs = root->bs;
    if( lsmash_bs_read_data( bs, 78 ) )
        return -1;
    for( int i = 0; i < 6; i++ )
        visual->reserved[i]       = lsmash_bs_get_byte( bs );
    visual->data_reference_index  = lsmash_bs_get_be16( bs );
    visual->version               = lsmash_bs_get_be16( bs );
    visual->revision_level        = lsmash_bs_get_be16( bs );
    visual->vendor                = lsmash_bs_get_be32( bs );
    visual->temporalQuality       = lsmash_bs_get_be32( bs );
    visual->spatialQuality        = lsmash_bs_get_be32( bs );
    visual->width                 = lsmash_bs_get_be16( bs );
    visual->height                = lsmash_bs_get_be16( bs );
    visual->horizresolution       = lsmash_bs_get_be32( bs );
    visual->vertresolution        = lsmash_bs_get_be32( bs );
    visual->dataSize              = lsmash_bs_get_be32( bs );
    visual->frame_count           = lsmash_bs_get_be16( bs );
    for( int i = 0; i < 32; i++ )
        visual->compressorname[i] = lsmash_bs_get_byte( bs );
    visual->depth                 = lsmash_bs_get_be16( bs );
    visual->color_table_ID        = lsmash_bs_get_be16( bs );
    box->parent = parent;
    isom_box_common_copy( visual, box );
    if( isom_add_print_func( root, visual, level ) )
        return -1;
    return isom_read_children( root, box, visual, level );
}

static int isom_read_btrt( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    isom_create_box( btrt, parent, box->type );
    ((isom_visual_entry_t *)parent)->btrt = btrt;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    btrt->bufferSizeDB = lsmash_bs_get_be32( bs );
    btrt->maxBitrate   = lsmash_bs_get_be32( bs );
    btrt->avgBitrate   = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( btrt, box );
    return isom_add_print_func( root, btrt, level );
}

static int isom_read_clap( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    isom_create_box( clap, parent, box->type );
    ((isom_visual_entry_t *)parent)->clap = clap;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    clap->cleanApertureWidthN  = lsmash_bs_get_be32( bs );
    clap->cleanApertureWidthD  = lsmash_bs_get_be32( bs );
    clap->cleanApertureHeightN = lsmash_bs_get_be32( bs );
    clap->cleanApertureHeightD = lsmash_bs_get_be32( bs );
    clap->horizOffN            = lsmash_bs_get_be32( bs );
    clap->horizOffD            = lsmash_bs_get_be32( bs );
    clap->vertOffN             = lsmash_bs_get_be32( bs );
    clap->vertOffD             = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( clap, box );
    return isom_add_print_func( root, clap, level );
}

static int isom_read_pasp( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    isom_create_box( pasp, parent, box->type );
    ((isom_visual_entry_t *)parent)->pasp = pasp;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    pasp->hSpacing = lsmash_bs_get_be32( bs );
    pasp->vSpacing = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( pasp, box );
    return isom_add_print_func( root, pasp, level );
}

static int isom_read_colr( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    isom_create_box( colr, parent, box->type );
    ((isom_visual_entry_t *)parent)->colr = colr;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    colr->color_parameter_type = lsmash_bs_get_be32( bs );
    if( colr->color_parameter_type == QT_COLOR_PARAMETER_TYPE_NCLC )
    {
        colr->primaries_index         = lsmash_bs_get_be16( bs );
        colr->transfer_function_index = lsmash_bs_get_be16( bs );
        colr->matrix_index            = lsmash_bs_get_be16( bs );
    }
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( colr, box );
    return isom_add_print_func( root, colr, level );
}

static int isom_read_stsl( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    isom_create_box( stsl, parent, box->type );
    ((isom_visual_entry_t *)parent)->stsl = stsl;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    stsl->constraint_flag  = lsmash_bs_get_byte( bs );
    stsl->scale_method     = lsmash_bs_get_byte( bs );
    stsl->display_center_x = lsmash_bs_get_be16( bs );
    stsl->display_center_y = lsmash_bs_get_be16( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( stsl, box );
    return isom_add_print_func( root, stsl, level );
}

static int isom_read_avcC_ps( lsmash_bs_t *bs, lsmash_entry_list_t *list, uint8_t entry_count )
{
    if( !list )
        return -1;
    for( uint8_t i = 0; i < entry_count; i++ )
    {
        isom_avcC_ps_entry_t *data = malloc( sizeof(isom_avcC_ps_entry_t) );
        if( !data || lsmash_add_entry( list, data ) )
            return -1;      /* don't free list, here */
        data->parameterSetLength  = lsmash_bs_get_be16( bs );
        data->parameterSetNALUnit = lsmash_bs_get_bytes( bs, data->parameterSetLength );
        if( !data->parameterSetNALUnit )
            return -1;      /* don't free list, here */
    }
    return 0;
}

static int isom_read_avcC( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    isom_create_box( avcC, parent, box->type );
    isom_visual_entry_t *visual = (isom_visual_entry_t *)parent;
    visual->avcC = avcC;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    avcC->configurationVersion       = lsmash_bs_get_byte( bs );
    avcC->AVCProfileIndication       = lsmash_bs_get_byte( bs );
    avcC->profile_compatibility      = lsmash_bs_get_byte( bs );
    avcC->AVCLevelIndication         = lsmash_bs_get_byte( bs );
    avcC->lengthSizeMinusOne         = lsmash_bs_get_byte( bs );
    avcC->numOfSequenceParameterSets = lsmash_bs_get_byte( bs );
    if( avcC->numOfSequenceParameterSets & 0x1f )
    {
        avcC->sequenceParameterSets = lsmash_create_entry_list();
        if( !avcC->sequenceParameterSets ||
            isom_read_avcC_ps( bs, avcC->sequenceParameterSets, avcC->numOfSequenceParameterSets & 0x1f ) )
            goto fail;
    }
    avcC->numOfPictureParameterSets  = lsmash_bs_get_byte( bs );
    if( avcC->numOfPictureParameterSets )
    {
        avcC->pictureParameterSets = lsmash_create_entry_list();
        if( !avcC->pictureParameterSets ||
            isom_read_avcC_ps( bs, avcC->pictureParameterSets, avcC->numOfPictureParameterSets ) )
            goto fail;
    }
    /* Note: there are too many files, in the world, that don't contain the following fields.*/
    if( ISOM_REQUIRES_AVCC_EXTENSION( avcC->AVCProfileIndication ) && lsmash_bs_get_pos( bs ) < box->size )
    {
        avcC->chroma_format                = lsmash_bs_get_byte( bs );
        avcC->bit_depth_luma_minus8        = lsmash_bs_get_byte( bs );
        avcC->bit_depth_chroma_minus8      = lsmash_bs_get_byte( bs );
        avcC->numOfSequenceParameterSetExt = lsmash_bs_get_byte( bs );
        if( avcC->numOfSequenceParameterSetExt )
        {
            avcC->sequenceParameterSetExt = lsmash_create_entry_list();
            if( !avcC->sequenceParameterSetExt ||
                isom_read_avcC_ps( bs, avcC->sequenceParameterSetExt, avcC->numOfSequenceParameterSetExt ) )
                goto fail;
        }
    }
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( avcC, box );
    return isom_add_print_func( root, avcC, level );
fail:
    lsmash_remove_list( avcC->sequenceParameterSets,   isom_remove_avcC_ps );
    lsmash_remove_list( avcC->pictureParameterSets,    isom_remove_avcC_ps );
    lsmash_remove_list( avcC->sequenceParameterSetExt, isom_remove_avcC_ps );
    free( avcC );
    visual->avcC = NULL;
    return -1;
}

static int isom_read_audio_description( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STSD )
        return isom_read_unknown_box( root, box, parent, level );
    isom_audio_entry_t *audio = (isom_audio_entry_t *)isom_add_description( box->type, ((isom_stsd_t *)parent)->list );
    if( !audio )
        return -1;
    lsmash_bs_t *bs = root->bs;
    if( lsmash_bs_read_data( bs, 28 ) )
        return -1;
    for( int i = 0; i < 6; i++ )
        audio->reserved[i]      = lsmash_bs_get_byte( bs );
    audio->data_reference_index = lsmash_bs_get_be16( bs );
    audio->version              = lsmash_bs_get_be16( bs );
    audio->revision_level       = lsmash_bs_get_be16( bs );
    audio->vendor               = lsmash_bs_get_be32( bs );
    audio->channelcount         = lsmash_bs_get_be16( bs );
    audio->samplesize           = lsmash_bs_get_be16( bs );
    audio->compression_ID       = lsmash_bs_get_be16( bs );
    audio->packet_size          = lsmash_bs_get_be16( bs );
    audio->samplerate           = lsmash_bs_get_be32( bs );
    if( audio->version == 1 )
    {
        if( lsmash_bs_read_data( bs, 16 ) )
            return -1;
        audio->samplesPerPacket = lsmash_bs_get_be32( bs );
        audio->bytesPerPacket   = lsmash_bs_get_be32( bs );
        audio->bytesPerFrame    = lsmash_bs_get_be32( bs );
        audio->bytesPerSample   = lsmash_bs_get_be32( bs );
    }
    else if( audio->version == 2 )
    {
        if( lsmash_bs_read_data( bs, 36 ) )
            return -1;
        audio->sizeOfStructOnly              = lsmash_bs_get_be32( bs );
        audio->audioSampleRate               = lsmash_bs_get_be64( bs );
        audio->numAudioChannels              = lsmash_bs_get_be32( bs );
        audio->always7F000000                = lsmash_bs_get_be32( bs );
        audio->constBitsPerChannel           = lsmash_bs_get_be32( bs );
        audio->formatSpecificFlags           = lsmash_bs_get_be32( bs );
        audio->constBytesPerAudioPacket      = lsmash_bs_get_be32( bs );
        audio->constLPCMFramesPerAudioPacket = lsmash_bs_get_be32( bs );
    }
    box->parent = parent;
    isom_box_common_copy( audio, box );
    if( isom_add_print_func( root, audio, level ) )
        return -1;
    return isom_read_children( root, box, audio, level );
}

static int isom_read_wave( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    isom_create_box( wave, parent, box->type );
    ((isom_audio_entry_t *)parent)->wave = wave;
    isom_box_common_copy( wave, box );
    if( isom_add_print_func( root, wave, level ) )
        return -1;
    return isom_read_children( root, box, wave, level );
}

static int isom_read_frma( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != QT_BOX_TYPE_WAVE )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( frma, parent, box->type );
    ((isom_wave_t *)parent)->frma = frma;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    frma->data_format = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( frma, box );
    return isom_add_print_func( root, frma, level );
}

static int isom_read_enda( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != QT_BOX_TYPE_WAVE )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( enda, parent, box->type );
    ((isom_wave_t *)parent)->enda = enda;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    enda->littleEndian = lsmash_bs_get_be16( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( enda, box );
    return isom_add_print_func( root, enda, level );
}

static int isom_read_audio_specific( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != QT_BOX_TYPE_WAVE )
        return isom_read_unknown_box( root, box, parent, level );
    isom_box_t *specific = malloc( sizeof(isom_box_t) );
    if( !specific )
        return -1;
    memset( specific, 0, sizeof(isom_box_t) );
    lsmash_bs_t *bs = root->bs;
    isom_skip_box_rest( bs, box );
    box->manager |= 0x02;       /* add free flag */
    isom_box_common_copy( specific, box );
    if( isom_add_print_func( root, specific, level ) )
    {
        free( specific );
        return -1;
    }
    return 0;
}

static int isom_read_terminator( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != QT_BOX_TYPE_WAVE )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( terminator, parent, box->type );
    ((isom_wave_t *)parent)->terminator = terminator;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( terminator, box );
    return isom_add_print_func( root, terminator, level );
}

static int isom_read_chan( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    isom_create_box( chan, parent, box->type );
    isom_audio_entry_t *audio = (isom_audio_entry_t *)parent;
    audio->chan = chan;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    chan->channelLayoutTag          = lsmash_bs_get_be32( bs );
    chan->channelBitmap             = lsmash_bs_get_be32( bs );
    chan->numberChannelDescriptions = lsmash_bs_get_be32( bs );
    if( chan->numberChannelDescriptions )
    {
        isom_channel_description_t *desc = malloc( chan->numberChannelDescriptions * sizeof(isom_channel_description_t) );
        if( !desc )
        {
            free( chan );
            audio->chan = NULL;
            return -1;
        }
        chan->channelDescriptions = desc;
        for( uint32_t i = 0; i < chan->numberChannelDescriptions; i++ )
        {
            desc->channelLabel       = lsmash_bs_get_be32( bs );
            desc->channelFlags       = lsmash_bs_get_be32( bs );
            for( int j = 0; j < 3; j++ )
                desc->coordinates[j] = lsmash_bs_get_be32( bs );
        }
    }
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( chan, box );
    return isom_add_print_func( root, chan, level );
}

static int isom_read_text_description( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STSD )
        return isom_read_unknown_box( root, box, parent, level );
    isom_text_entry_t *text = (isom_text_entry_t *)isom_add_description( box->type, ((isom_stsd_t *)parent)->list );
    if( !text )
        return -1;
    lsmash_bs_t *bs = root->bs;
    if( lsmash_bs_read_data( bs, 51 ) )
        return -1;
    for( int i = 0; i < 6; i++ )
        text->reserved[i]        = lsmash_bs_get_byte( bs );
    text->data_reference_index   = lsmash_bs_get_be16( bs );
    text->displayFlags           = lsmash_bs_get_be32( bs );
    text->textJustification      = lsmash_bs_get_be32( bs );
    for( int i = 0; i < 3; i++ )
        text->bgColor[i]         = lsmash_bs_get_be16( bs );
    text->top                    = lsmash_bs_get_be16( bs );
    text->left                   = lsmash_bs_get_be16( bs );
    text->bottom                 = lsmash_bs_get_be16( bs );
    text->right                  = lsmash_bs_get_be16( bs );
    text->scrpStartChar          = lsmash_bs_get_be32( bs );
    text->scrpHeight             = lsmash_bs_get_be16( bs );
    text->scrpAscent             = lsmash_bs_get_be16( bs );
    text->scrpFont               = lsmash_bs_get_be16( bs );
    text->scrpFace               = lsmash_bs_get_be16( bs );
    text->scrpSize               = lsmash_bs_get_be16( bs );
    for( int i = 0; i < 3; i++ )
        text->scrpColor[i]       = lsmash_bs_get_be16( bs );
    text->font_name_length       = lsmash_bs_get_byte( bs );
    if( text->font_name_length )
    {
        if( lsmash_bs_read_data( bs, text->font_name_length ) )
            return -1;
        text->font_name = malloc( text->font_name_length + 1 );
        if( !text->font_name )
            return -1;
        for( uint8_t i = 0; i < text->font_name_length; i++ )
            text->font_name[i] = lsmash_bs_get_byte( bs );
        text->font_name[text->font_name_length] = '\0';
    }
    box->parent = parent;
    isom_box_common_copy( text, box );
    if( isom_add_print_func( root, text, level ) )
        return -1;
    return isom_read_children( root, box, text, level );
}

static int isom_read_tx3g_description( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STSD )
        return isom_read_unknown_box( root, box, parent, level );
    isom_tx3g_entry_t *tx3g = (isom_tx3g_entry_t *)isom_add_description( box->type, ((isom_stsd_t *)parent)->list );
    if( !tx3g )
        return -1;
    lsmash_bs_t *bs = root->bs;
    if( lsmash_bs_read_data( bs, 38 ) )
        return -1;
    for( int i = 0; i < 6; i++ )
        tx3g->reserved[i]              = lsmash_bs_get_byte( bs );
    tx3g->data_reference_index         = lsmash_bs_get_be16( bs );
    tx3g->displayFlags                 = lsmash_bs_get_be32( bs );
    tx3g->horizontal_justification     = lsmash_bs_get_byte( bs );
    tx3g->vertical_justification       = lsmash_bs_get_byte( bs );
    for( int i = 0; i < 4; i++ )
        tx3g->background_color_rgba[i] = lsmash_bs_get_byte( bs );
    tx3g->top                          = lsmash_bs_get_be16( bs );
    tx3g->left                         = lsmash_bs_get_be16( bs );
    tx3g->bottom                       = lsmash_bs_get_be16( bs );
    tx3g->right                        = lsmash_bs_get_be16( bs );
    tx3g->startChar                    = lsmash_bs_get_be16( bs );
    tx3g->endChar                      = lsmash_bs_get_be16( bs );
    tx3g->font_ID                      = lsmash_bs_get_be16( bs );
    tx3g->face_style_flags             = lsmash_bs_get_byte( bs );
    tx3g->font_size                    = lsmash_bs_get_byte( bs );
    for( int i = 0; i < 4; i++ )
        tx3g->text_color_rgba[i]       = lsmash_bs_get_byte( bs );
    box->parent = parent;
    isom_box_common_copy( tx3g, box );
    if( isom_add_print_func( root, tx3g, level ) )
        return -1;
    return isom_read_children( root, box, tx3g, level );
}

static int isom_read_ftab( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_CODEC_TYPE_TX3G_TEXT )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_list_box( ftab, parent, box->type );
    ((isom_tx3g_entry_t *)parent)->ftab = ftab;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    uint32_t entry_count = lsmash_bs_get_be16( bs );
    uint64_t pos;
    for( pos = lsmash_bs_get_pos( bs ); pos < box->size; pos = lsmash_bs_get_pos( bs ) )
    {
        isom_font_record_t *data = malloc( sizeof(isom_font_record_t) );
        if( !data || lsmash_add_entry( ftab->list, data ) )
        {
            if( data )
                free( data );
            return -1;
        }
        memset( data, 0, sizeof(isom_font_record_t) );
        data->font_ID          = lsmash_bs_get_be16( bs );
        data->font_name_length = lsmash_bs_get_byte( bs );
        if( data->font_name_length )
        {
            data->font_name = malloc( data->font_name_length + 1 );
            if( !data->font_name )
                return -1;
            for( uint8_t i = 0; i < data->font_name_length; i++ )
                data->font_name[i] = lsmash_bs_get_byte( bs );
            data->font_name[data->font_name_length] = '\0';
        }
    }
    if( entry_count != ftab->list->entry_count || box->size < pos )
        printf( "[ftab] box has extra bytes: %"PRId64"\n", pos - box->size );
    box->size = pos;
    isom_box_common_copy( ftab, box );
    return isom_add_print_func( root, ftab, level );
}

static int isom_read_stts( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STBL )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_list_box( stts, parent, box->type );
    ((isom_stbl_t *)parent)->stts = stts;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    uint32_t entry_count = lsmash_bs_get_be32( bs );
    uint64_t pos;
    for( pos = lsmash_bs_get_pos( bs ); pos < box->size; pos = lsmash_bs_get_pos( bs ) )
    {
        isom_stts_entry_t *data = malloc( sizeof(isom_stts_entry_t) );
        if( !data || lsmash_add_entry( stts->list, data ) )
        {
            if( data )
                free( data );
            return -1;
        }
        memset( data, 0, sizeof(isom_stts_entry_t) );
        data->sample_count = lsmash_bs_get_be32( bs );
        data->sample_delta = lsmash_bs_get_be32( bs );
    }
    if( entry_count != stts->list->entry_count || box->size < pos )
        printf( "[stts] box has extra bytes: %"PRId64"\n", pos - box->size );
    box->size = pos;
    isom_box_common_copy( stts, box );
    return isom_add_print_func( root, stts, level );
}

static int isom_read_ctts( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STBL )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_list_box( ctts, parent, box->type );
    ((isom_stbl_t *)parent)->ctts = ctts;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    uint32_t entry_count = lsmash_bs_get_be32( bs );
    uint64_t pos;
    for( pos = lsmash_bs_get_pos( bs ); pos < box->size; pos = lsmash_bs_get_pos( bs ) )
    {
        isom_ctts_entry_t *data = malloc( sizeof(isom_ctts_entry_t) );
        if( !data || lsmash_add_entry( ctts->list, data ) )
        {
            if( data )
                free( data );
            return -1;
        }
        memset( data, 0, sizeof(isom_ctts_entry_t) );
        data->sample_count  = lsmash_bs_get_be32( bs );
        data->sample_offset = lsmash_bs_get_be32( bs );
    }
    if( entry_count != ctts->list->entry_count || box->size < pos )
        printf( "[ctts] box has extra bytes: %"PRId64"\n", pos - box->size );
    box->size = pos;
    isom_box_common_copy( ctts, box );
    return isom_add_print_func( root, ctts, level );
}

static int isom_read_cslg( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STBL )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( cslg, parent, box->type );
    ((isom_stbl_t *)parent)->cslg = cslg;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    cslg->compositionToDTSShift        = lsmash_bs_get_be32( bs );
    cslg->leastDecodeToDisplayDelta    = lsmash_bs_get_be32( bs );
    cslg->greatestDecodeToDisplayDelta = lsmash_bs_get_be32( bs );
    cslg->compositionStartTime         = lsmash_bs_get_be32( bs );
    cslg->compositionEndTime           = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( cslg, box );
    return isom_add_print_func( root, cslg, level );
}

static int isom_read_stss( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STBL )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_list_box( stss, parent, box->type );
    ((isom_stbl_t *)parent)->stss = stss;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    uint32_t entry_count = lsmash_bs_get_be32( bs );
    uint64_t pos;
    for( pos = lsmash_bs_get_pos( bs ); pos < box->size; pos = lsmash_bs_get_pos( bs ) )
    {
        isom_stss_entry_t *data = malloc( sizeof(isom_stss_entry_t) );
        if( !data || lsmash_add_entry( stss->list, data ) )
        {
            if( data )
                free( data );
            return -1;
        }
        memset( data, 0, sizeof(isom_stss_entry_t) );
        data->sample_number = lsmash_bs_get_be32( bs );
    }
    if( entry_count != stss->list->entry_count || box->size < pos )
        printf( "[stss] box has extra bytes: %"PRId64"\n", pos - box->size );
    box->size = pos;
    isom_box_common_copy( stss, box );
    return isom_add_print_func( root, stss, level );
}

static int isom_read_stps( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STBL )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_list_box( stps, parent, box->type );
    ((isom_stbl_t *)parent)->stps = stps;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    uint32_t entry_count = lsmash_bs_get_be32( bs );
    uint64_t pos;
    for( pos = lsmash_bs_get_pos( bs ); pos < box->size; pos = lsmash_bs_get_pos( bs ) )
    {
        isom_stps_entry_t *data = malloc( sizeof(isom_stps_entry_t) );
        if( !data || lsmash_add_entry( stps->list, data ) )
        {
            if( data )
                free( data );
            return -1;
        }
        memset( data, 0, sizeof(isom_stps_entry_t) );
        data->sample_number = lsmash_bs_get_be32( bs );
    }
    if( entry_count != stps->list->entry_count || box->size < pos )
        printf( "[stps] box has extra bytes: %"PRId64"\n", pos - box->size );
    box->size = pos;
    isom_box_common_copy( stps, box );
    return isom_add_print_func( root, stps, level );
}

static int isom_read_sdtp( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STBL )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_list_box( sdtp, parent, box->type );
    ((isom_stbl_t *)parent)->sdtp = sdtp;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    uint64_t pos;
    for( pos = lsmash_bs_get_pos( bs ); pos < box->size; pos = lsmash_bs_get_pos( bs ) )
    {
        isom_sdtp_entry_t *data = malloc( sizeof(isom_sdtp_entry_t) );
        if( !data || lsmash_add_entry( sdtp->list, data ) )
        {
            if( data )
                free( data );
            return -1;
        }
        memset( data, 0, sizeof(isom_sdtp_entry_t) );
        uint8_t temp = lsmash_bs_get_byte( bs );
        data->is_leading            = (temp >> 6) & 0x3;
        data->sample_depends_on     = (temp >> 4) & 0x3;
        data->sample_is_depended_on = (temp >> 2) & 0x3;
        data->sample_has_redundancy =  temp       & 0x3;
    }
    box->size = pos;
    isom_box_common_copy( sdtp, box );
    return isom_add_print_func( root, sdtp, level );
}

static int isom_read_stsc( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STBL )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_list_box( stsc, parent, box->type );
    ((isom_stbl_t *)parent)->stsc = stsc;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    uint32_t entry_count = lsmash_bs_get_be32( bs );
    uint64_t pos;
    for( pos = lsmash_bs_get_pos( bs ); pos < box->size; pos = lsmash_bs_get_pos( bs ) )
    {
        isom_stsc_entry_t *data = malloc( sizeof(isom_stsc_entry_t) );
        if( !data || lsmash_add_entry( stsc->list, data ) )
        {
            if( data )
                free( data );
            return -1;
        }
        memset( data, 0, sizeof(isom_stsc_entry_t) );
        data->first_chunk              = lsmash_bs_get_be32( bs );
        data->samples_per_chunk        = lsmash_bs_get_be32( bs );
        data->sample_description_index = lsmash_bs_get_be32( bs );
    }
    if( entry_count != stsc->list->entry_count || box->size < pos )
        printf( "[stsc] box has extra bytes: %"PRId64"\n", pos - box->size );
    box->size = pos;
    isom_box_common_copy( stsc, box );
    return isom_add_print_func( root, stsc, level );
}

static int isom_read_stsz( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STBL )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( stsz, parent, box->type );
    ((isom_stbl_t *)parent)->stsz = stsz;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    stsz->sample_size  = lsmash_bs_get_be32( bs );
    stsz->sample_count = lsmash_bs_get_be32( bs );
    uint64_t pos = lsmash_bs_get_pos( bs );
    if( pos < box->size )
    {
        stsz->list = lsmash_create_entry_list();
        if( !stsz->list )
            return -1;
        for( ; pos < box->size; pos = lsmash_bs_get_pos( bs ) )
        {
            isom_stsz_entry_t *data = malloc( sizeof(isom_stsz_entry_t) );
            if( !data || lsmash_add_entry( stsz->list, data ) )
            {
                if( data )
                    free( data );
                return -1;
            }
            memset( data, 0, sizeof(isom_stsz_entry_t) );
            data->entry_size = lsmash_bs_get_be32( bs );
        }
    }
    if( (stsz->list && stsz->sample_count != stsz->list->entry_count) || box->size < pos )
        printf( "[stsz] box has extra bytes: %"PRId64"\n", pos - box->size );
    box->size = pos;
    isom_box_common_copy( stsz, box );
    return isom_add_print_func( root, stsz, level );
}

static int isom_read_stco( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STBL )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_list_box( stco, parent, box->type );
    ((isom_stbl_t *)parent)->stco = stco;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    uint32_t entry_count = lsmash_bs_get_be32( bs );
    uint64_t pos;
    if( box->type == ISOM_BOX_TYPE_STCO )
        for( pos = lsmash_bs_get_pos( bs ); pos < box->size; pos = lsmash_bs_get_pos( bs ) )
        {
            isom_stco_entry_t *data = malloc( sizeof(isom_stco_entry_t) );
            if( !data || lsmash_add_entry( stco->list, data ) )
            {
                if( data )
                    free( data );
                return -1;
            }
            memset( data, 0, sizeof(isom_stco_entry_t) );
            data->chunk_offset = lsmash_bs_get_be32( bs );
        }
    else
        for( pos = lsmash_bs_get_pos( bs ); pos < box->size; pos = lsmash_bs_get_pos( bs ) )
        {
            isom_co64_entry_t *data = malloc( sizeof(isom_co64_entry_t) );
            if( !data || lsmash_add_entry( stco->list, data ) )
            {
                if( data )
                    free( data );
                return -1;
            }
            memset( data, 0, sizeof(isom_co64_entry_t) );
            data->chunk_offset = lsmash_bs_get_be64( bs );
        }
    if( entry_count != stco->list->entry_count || box->size < pos )
        printf( "[%s] box has extra bytes: %"PRId64"\n", isom_4cc2str( box->type ), pos - box->size );
    box->size = pos;
    isom_box_common_copy( stco, box );
    return isom_add_print_func( root, stco, level );
}

static int isom_read_sgpd( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STBL )
        return isom_read_unknown_box( root, box, parent, level );
    isom_stbl_t *stbl = (isom_stbl_t *)parent;
    lsmash_entry_list_t *list = stbl->sgpd_list;
    if( !list )
    {
        list = lsmash_create_entry_list();
        if( !list )
            return -1;
        stbl->sgpd_list = list;
    }
    isom_sgpd_entry_t *sgpd = malloc( sizeof(isom_sgpd_entry_t) );
    if( !sgpd )
        return -1;
    memset( sgpd, 0, sizeof(isom_sgpd_entry_t) );
    sgpd->list = lsmash_create_entry_list();
    if( !sgpd->list || lsmash_add_entry( list, sgpd ) )
    {
        free( sgpd );
        return -1;
    }
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    sgpd->grouping_type      = lsmash_bs_get_be32( bs );
    if( box->version == 1 )
        sgpd->default_length = lsmash_bs_get_be32( bs );
    uint32_t entry_count     = lsmash_bs_get_be32( bs );
    switch( sgpd->grouping_type )
    {
        case ISOM_GROUP_TYPE_RAP :
        {
            uint64_t pos;
            for( pos = lsmash_bs_get_pos( bs ); pos < box->size; pos = lsmash_bs_get_pos( bs ) )
            {
                isom_rap_entry_t *data = malloc( sizeof(isom_rap_entry_t) );
                if( !data || lsmash_add_entry( sgpd->list, data ) )
                {
                    if( data )
                        free( data );
                    return -1;
                }
                memset( data, 0, sizeof(isom_rap_entry_t) );
                /* We don't know groups decided by variable description length. If encountering, skip getting of bytes of it. */
                if( box->version == 1 && !sgpd->default_length )
                    data->description_length = lsmash_bs_get_be32( bs );
                else
                {
                    uint8_t temp = lsmash_bs_get_byte( bs );
                    data->num_leading_samples_known = (temp >> 7) & 0x01;
                    data->num_leading_samples       =  temp       & 0x7f;
                }
            }
            if( entry_count != sgpd->list->entry_count || box->size < pos )
                printf( "[sgpd] box has extra bytes: %"PRId64"\n", pos - box->size );
            box->size = pos;
            break;
        }
        case ISOM_GROUP_TYPE_ROLL :
        {
            uint64_t pos;
            for( pos = lsmash_bs_get_pos( bs ); pos < box->size; pos = lsmash_bs_get_pos( bs ) )
            {
                isom_roll_entry_t *data = malloc( sizeof(isom_roll_entry_t) );
                if( !data || lsmash_add_entry( sgpd->list, data ) )
                {
                    if( data )
                        free( data );
                    return -1;
                }
                memset( data, 0, sizeof(isom_roll_entry_t) );
                /* We don't know groups decided by variable description length. If encountering, skip getting of bytes of it. */
                if( box->version == 1 && !sgpd->default_length )
                    data->description_length = lsmash_bs_get_be32( bs );
                else
                    data->roll_distance      = lsmash_bs_get_be16( bs );
            }
            if( entry_count != sgpd->list->entry_count || box->size < pos )
                printf( "[sgpd] box has extra bytes: %"PRId64"\n", pos - box->size );
            box->size = pos;
            break;
        }
        default :
            break;
    }
    isom_box_common_copy( sgpd, box );
    return isom_add_print_func( root, sgpd, level );
}

static int isom_read_sbgp( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_STBL && parent->type != ISOM_BOX_TYPE_TRAF )
        return isom_read_unknown_box( root, box, parent, level );
    isom_stbl_t *stbl = (isom_stbl_t *)parent;
    lsmash_entry_list_t *list = stbl->sbgp_list;
    if( !list )
    {
        list = lsmash_create_entry_list();
        if( !list )
            return -1;
        stbl->sbgp_list = list;
    }
    isom_sbgp_entry_t *sbgp = malloc( sizeof(isom_sbgp_entry_t) );
    if( !sbgp )
        return -1;
    memset( sbgp, 0, sizeof(isom_sbgp_entry_t) );
    sbgp->list = lsmash_create_entry_list();
    if( !sbgp->list || lsmash_add_entry( list, sbgp ) )
    {
        free( sbgp );
        return -1;
    }
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    sbgp->grouping_type  = lsmash_bs_get_be32( bs );
    if( sbgp->version == 1 )
        sbgp->grouping_type_parameter = lsmash_bs_get_be32( bs );
    uint32_t entry_count = lsmash_bs_get_be32( bs );
    uint64_t pos;
    for( pos = lsmash_bs_get_pos( bs ); pos < box->size; pos = lsmash_bs_get_pos( bs ) )
    {
        isom_group_assignment_entry_t *data = malloc( sizeof(isom_group_assignment_entry_t) );
        if( !data || lsmash_add_entry( sbgp->list, data ) )
        {
            if( data )
                free( data );
            return -1;
        }
        memset( data, 0, sizeof(isom_group_assignment_entry_t) );
        data->sample_count            = lsmash_bs_get_be32( bs );
        data->group_description_index = lsmash_bs_get_be32( bs );
    }
    if( entry_count != sbgp->list->entry_count || box->size < pos )
        printf( "[sbgp] box has extra bytes: %"PRId64"\n", pos - box->size );
    box->size = pos;
    isom_box_common_copy( sbgp, box );
    return isom_add_print_func( root, sbgp, level );
}

static int isom_read_udta( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MOOV && parent->type != ISOM_BOX_TYPE_TRAK )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( udta, parent, box->type );
    if( parent->type == ISOM_BOX_TYPE_MOOV )
        ((isom_moov_t *)parent)->udta = udta;
    else
        ((isom_trak_entry_t *)parent)->udta = udta;
    isom_box_common_copy( udta, box );
    if( isom_add_print_func( root, udta, level ) )
        return -1;
    return isom_read_children( root, box, udta, level );
}

static int isom_read_chpl( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_UDTA )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_list_box( chpl, parent, box->type );
    ((isom_udta_t *)parent)->chpl = chpl;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    uint32_t entry_count;
    if( box->version == 1 )
    {
        chpl->unknown = lsmash_bs_get_byte( bs );
        entry_count   = lsmash_bs_get_be32( bs );
    }
    else
        entry_count   = lsmash_bs_get_byte( bs );
    uint64_t pos;
    for( pos = lsmash_bs_get_pos( bs ); pos < box->size; pos = lsmash_bs_get_pos( bs ) )
    {
        isom_chpl_entry_t *data = malloc( sizeof(isom_chpl_entry_t) );
        if( !data || lsmash_add_entry( chpl->list, data ) )
        {
            if( data )
                free( data );
            return -1;
        }
        memset( data, 0, sizeof(isom_chpl_entry_t) );
        data->start_time          = lsmash_bs_get_be64( bs );
        data->chapter_name_length = lsmash_bs_get_byte( bs );
        data->chapter_name = malloc( data->chapter_name_length + 1 );
        if( !data->chapter_name )
        {
            free( data );
            return -1;
        }
        for( uint8_t i = 0; i < data->chapter_name_length; i++ )
            data->chapter_name[i] = lsmash_bs_get_byte( bs );
        data->chapter_name[data->chapter_name_length] = '\0';
    }
    if( entry_count != chpl->list->entry_count || box->size < pos )
        printf( "[chpl] box has extra bytes: %"PRId64"\n", pos - box->size );
    box->size = pos;
    isom_box_common_copy( chpl, box );
    return isom_add_print_func( root, chpl, level );
}

static int isom_read_mvex( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MOOV )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( mvex, parent, box->type );
    ((isom_moov_t *)parent)->mvex = mvex;
    isom_box_common_copy( mvex, box );
    if( isom_add_print_func( root, mvex, level ) )
        return -1;
    return isom_read_children( root, box, mvex, level );
}

static int isom_read_mehd( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MVEX )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( mehd, parent, box->type );
    ((isom_mvex_t *)parent)->mehd = mehd;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    if( box->version == 1 )
        mehd->fragment_duration = lsmash_bs_get_be64( bs );
    else
        mehd->fragment_duration = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( mehd, box );
    return isom_add_print_func( root, mehd, level );
}

static isom_sample_flags_t isom_bs_get_sample_flags( lsmash_bs_t *bs )
{
    uint32_t temp = lsmash_bs_get_be32( bs );
    isom_sample_flags_t flags;
    flags.reserved                    = (temp >> 28) & 0xf;
    flags.is_leading                  = (temp >> 26) & 0x3;
    flags.sample_depends_on           = (temp >> 24) & 0x3;
    flags.sample_is_depended_on       = (temp >> 22) & 0x3;
    flags.sample_has_redundancy       = (temp >> 20) & 0x3;
    flags.sample_padding_value        = (temp >> 17) & 0x7;
    flags.sample_is_non_sync_sample   = (temp >> 16) & 0x1;
    flags.sample_degradation_priority =  temp        & 0xffff;
    return flags;
}

static int isom_read_trex( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MVEX )
        return isom_read_unknown_box( root, box, parent, level );
    lsmash_entry_list_t *list = ((isom_mvex_t *)parent)->trex_list;
    if( !list )
    {
        list = lsmash_create_entry_list();
        if( !list )
            return -1;
        ((isom_mvex_t *)parent)->trex_list = list;
    }
    isom_trex_entry_t *trex = malloc( sizeof(isom_trex_entry_t) );
    if( !trex )
        return -1;
    memset( trex, 0, sizeof(isom_trex_entry_t) );
    if( lsmash_add_entry( list, trex ) )
    {
        free( trex );
        return -1;
    }
    box->parent = parent;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    trex->track_ID                         = lsmash_bs_get_be32( bs );
    trex->default_sample_description_index = lsmash_bs_get_be32( bs );
    trex->default_sample_duration          = lsmash_bs_get_be32( bs );
    trex->default_sample_size              = lsmash_bs_get_be32( bs );
    trex->default_sample_flags             = isom_bs_get_sample_flags( bs );
    isom_box_common_copy( trex, box );
    return isom_add_print_func( root, trex, level );
}

static int isom_read_moof( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( !!parent->type )
        return isom_read_unknown_box( root, box, parent, level );
    lsmash_entry_list_t *list = ((lsmash_root_t *)parent)->moof_list;
    if( !list )
    {
        list = lsmash_create_entry_list();
        if( !list )
            return -1;
        ((lsmash_root_t *)parent)->moof_list = list;
    }
    isom_moof_entry_t *moof = malloc( sizeof(isom_moof_entry_t) );
    if( !moof )
        return -1;
    memset( moof, 0, sizeof(isom_moof_entry_t) );
    if( lsmash_add_entry( list, moof ) )
    {
        free( moof );
        return -1;
    }
    box->parent = parent;
    isom_box_common_copy( moof, box );
    if( isom_add_print_func( root, moof, level ) )
        return -1;
    return isom_read_children( root, box, moof, level );
}

static int isom_read_mfhd( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MOOF )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( mfhd, parent, box->type );
    ((isom_moof_entry_t *)parent)->mfhd = mfhd;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    mfhd->sequence_number = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( mfhd, box );
    return isom_add_print_func( root, mfhd, level );
}

static int isom_read_traf( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MOOF )
        return isom_read_unknown_box( root, box, parent, level );
    lsmash_entry_list_t *list = ((isom_moof_entry_t *)parent)->traf_list;
    if( !list )
    {
        list = lsmash_create_entry_list();
        if( !list )
            return -1;
        ((isom_moof_entry_t *)parent)->traf_list = list;
    }
    isom_traf_entry_t *traf = malloc( sizeof(isom_traf_entry_t) );
    if( !traf )
        return -1;
    memset( traf, 0, sizeof(isom_traf_entry_t) );
    if( lsmash_add_entry( list, traf ) )
    {
        free( traf );
        return -1;
    }
    box->parent = parent;
    isom_box_common_copy( traf, box );
    if( isom_add_print_func( root, traf, level ) )
        return -1;
    return isom_read_children( root, box, traf, level );
}

static int isom_read_tfhd( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_TRAF )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( tfhd, parent, box->type );
    ((isom_traf_entry_t *)parent)->tfhd = tfhd;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    tfhd->track_ID = lsmash_bs_get_be32( bs );
    if( box->flags & ISOM_TF_FLAGS_BASE_DATA_OFFSET_PRESENT         ) tfhd->base_data_offset         = lsmash_bs_get_be64( bs );
    if( box->flags & ISOM_TF_FLAGS_SAMPLE_DESCRIPTION_INDEX_PRESENT ) tfhd->sample_description_index = lsmash_bs_get_be32( bs );
    if( box->flags & ISOM_TF_FLAGS_DEFAULT_SAMPLE_DURATION_PRESENT  ) tfhd->default_sample_duration  = lsmash_bs_get_be32( bs );
    if( box->flags & ISOM_TF_FLAGS_DEFAULT_SAMPLE_SIZE_PRESENT      ) tfhd->default_sample_size      = lsmash_bs_get_be32( bs );
    if( box->flags & ISOM_TF_FLAGS_DEFAULT_SAMPLE_FLAGS_PRESENT     ) tfhd->default_sample_flags     = isom_bs_get_sample_flags( bs );
    uint32_t pos = lsmash_bs_get_pos( bs );
    if( box->size < pos )
        printf( "[tfhd] box has extra bytes: %"PRId64"\n", pos - box->size );
    box->size = pos;
    isom_box_common_copy( tfhd, box );
    return isom_add_print_func( root, tfhd, level );
}

static int isom_read_trun( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_TRAF )
        return isom_read_unknown_box( root, box, parent, level );
    lsmash_entry_list_t *list = ((isom_traf_entry_t *)parent)->trun_list;
    if( !list )
    {
        list = lsmash_create_entry_list();
        if( !list )
            return -1;
        ((isom_traf_entry_t *)parent)->trun_list = list;
    }
    isom_trun_entry_t *trun = malloc( sizeof(isom_trun_entry_t) );
    if( !trun )
        return -1;
    memset( trun, 0, sizeof(isom_trun_entry_t) );
    if( lsmash_add_entry( list, trun ) )
    {
        free( trun );
        return -1;
    }
    box->parent = parent;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    int has_optional_rows = ISOM_TR_FLAGS_SAMPLE_DURATION_PRESENT
                          | ISOM_TR_FLAGS_SAMPLE_SIZE_PRESENT
                          | ISOM_TR_FLAGS_SAMPLE_FLAGS_PRESENT
                          | ISOM_TR_FLAGS_SAMPLE_COMPOSITION_TIME_OFFSET_PRESENT;
    has_optional_rows &= box->flags;
    trun->sample_count = lsmash_bs_get_be32( bs );
    if( box->flags & ISOM_TR_FLAGS_DATA_OFFSET_PRESENT        ) trun->data_offset        = lsmash_bs_get_be32( bs );
    if( box->flags & ISOM_TR_FLAGS_FIRST_SAMPLE_FLAGS_PRESENT ) trun->first_sample_flags = isom_bs_get_sample_flags( bs );
    if( trun->sample_count && has_optional_rows )
    {
        trun->optional = lsmash_create_entry_list();
        if( !trun->optional )
            return -1;
        for( uint32_t i = 0; i < trun->sample_count; i++ )
        {
            isom_trun_optional_row_t *data = malloc( sizeof(isom_trun_optional_row_t) );
            if( !data || lsmash_add_entry( trun->optional, data ) )
            {
                if( data )
                    free( data );
                return -1;
            }
            memset( data, 0, sizeof(isom_trun_optional_row_t) );
            if( box->flags & ISOM_TR_FLAGS_SAMPLE_DURATION_PRESENT                ) data->sample_duration                = lsmash_bs_get_be32( bs );
            if( box->flags & ISOM_TR_FLAGS_SAMPLE_SIZE_PRESENT                    ) data->sample_size                    = lsmash_bs_get_be32( bs );
            if( box->flags & ISOM_TR_FLAGS_SAMPLE_FLAGS_PRESENT                   ) data->sample_flags                   = isom_bs_get_sample_flags( bs );
            if( box->flags & ISOM_TR_FLAGS_SAMPLE_COMPOSITION_TIME_OFFSET_PRESENT ) data->sample_composition_time_offset = lsmash_bs_get_be32( bs );
        }
    }
    uint32_t pos = lsmash_bs_get_pos( bs );
    if( box->size < pos )
        printf( "[trun] box has extra bytes: %"PRId64"\n", pos - box->size );
    box->size = pos;
    isom_box_common_copy( trun, box );
    return isom_add_print_func( root, trun, level );
}

static int isom_read_free( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    isom_box_t *skip = malloc( sizeof(isom_box_t) );
    if( !skip )
        return -1;
    memset( skip, 0, sizeof(isom_box_t) );
    lsmash_bs_t *bs = root->bs;
    isom_skip_box_rest( bs, box );
    box->manager |= 0x02;       /* add free flag */
    isom_box_common_copy( skip, box );
    if( isom_add_print_func( root, skip, level ) )
    {
        free( skip );
        return -1;
    }
    return 0;
}

static int isom_read_mdat( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( !!parent->type )
        return isom_read_unknown_box( root, box, parent, level );
    isom_box_t *mdat = malloc( sizeof(isom_box_t) );
    if( !mdat )
        return -1;
    memset( mdat, 0, sizeof(isom_box_t) );
    lsmash_bs_t *bs = root->bs;
    isom_skip_box_rest( bs, box );
    box->manager |= 0x02;       /* add free flag */
    isom_box_common_copy( mdat, box );
    if( isom_add_print_func( root, mdat, level ) )
    {
        free( mdat );
        return -1;
    }
    return 0;
}

static int isom_read_mfra( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( !!parent->type )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( mfra, parent, box->type );
    ((lsmash_root_t *)parent)->mfra = mfra;
    isom_box_common_copy( mfra, box );
    if( isom_add_print_func( root, mfra, level ) )
        return -1;
    return isom_read_children( root, box, mfra, level );
}

static int isom_read_tfra( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MFRA )
        return isom_read_unknown_box( root, box, parent, level );
    lsmash_entry_list_t *list = ((isom_mfra_t *)parent)->tfra_list;
    if( !list )
    {
        list = lsmash_create_entry_list();
        if( !list )
            return -1;
        ((isom_mfra_t *)parent)->tfra_list = list;
    }
    isom_tfra_entry_t *tfra = malloc( sizeof(isom_tfra_entry_t) );
    if( !tfra )
        return -1;
    memset( tfra, 0, sizeof(isom_tfra_entry_t) );
    if( lsmash_add_entry( list, tfra ) )
        goto fail;
    box->parent = parent;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    tfra->track_ID        = lsmash_bs_get_be32( bs );
    uint32_t temp         = lsmash_bs_get_be32( bs );
    tfra->number_of_entry = lsmash_bs_get_be32( bs );
    tfra->reserved                  = (temp >> 6) & 0x3ffffff;
    tfra->length_size_of_traf_num   = (temp >> 4) & 0x3;
    tfra->length_size_of_trun_num   = (temp >> 2) & 0x3;
    tfra->length_size_of_sample_num =  temp       & 0x3;
    if( tfra->number_of_entry )
    {
        tfra->list = lsmash_create_entry_list();
        if( !tfra->list )
            goto fail;
        uint64_t (*bs_get_funcs[5])( lsmash_bs_t * ) =
            {
              lsmash_bs_get_byte_to_64,
              lsmash_bs_get_be16_to_64,
              lsmash_bs_get_be24_to_64,
              lsmash_bs_get_be32_to_64,
              lsmash_bs_get_be64
            };
        uint64_t (*bs_put_time)         ( lsmash_bs_t * ) = bs_get_funcs[ 3 + (tfra->version == 1)        ];
        uint64_t (*bs_put_moof_offset)  ( lsmash_bs_t * ) = bs_get_funcs[ 3 + (tfra->version == 1)        ];
        uint64_t (*bs_put_traf_number)  ( lsmash_bs_t * ) = bs_get_funcs[ tfra->length_size_of_traf_num   ];
        uint64_t (*bs_put_trun_number)  ( lsmash_bs_t * ) = bs_get_funcs[ tfra->length_size_of_trun_num   ];
        uint64_t (*bs_put_sample_number)( lsmash_bs_t * ) = bs_get_funcs[ tfra->length_size_of_sample_num ];
        for( uint32_t i = 0; i < tfra->number_of_entry; i++ )
        {
            isom_tfra_location_time_entry_t *data = malloc( sizeof(isom_tfra_location_time_entry_t) );
            if( !data || lsmash_add_entry( tfra->list, data ) )
            {
                if( data )
                    free( data );
                goto fail;
            }
            memset( data, 0, sizeof(isom_tfra_location_time_entry_t) );
            data->time          = bs_put_time         ( bs );
            data->moof_offset   = bs_put_moof_offset  ( bs );
            data->traf_number   = bs_put_traf_number  ( bs );
            data->trun_number   = bs_put_trun_number  ( bs );
            data->sample_number = bs_put_sample_number( bs );
        }
    }
    uint32_t pos = lsmash_bs_get_pos( bs );
    if( (tfra->list && tfra->number_of_entry != tfra->list->entry_count) || box->size < pos )
        printf( "[tfra] box has extra bytes: %"PRId64"\n", pos - box->size );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( tfra, box );
    return isom_add_print_func( root, tfra, level );
fail:
    if( tfra->list )
        free( tfra->list );
    free( tfra );
    return -1;
}

static int isom_read_mfro( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, int level )
{
    if( parent->type != ISOM_BOX_TYPE_MFRA )
        return isom_read_unknown_box( root, box, parent, level );
    isom_create_box( mfro, parent, box->type );
    ((isom_mfra_t *)parent)->mfro = mfro;
    lsmash_bs_t *bs = root->bs;
    isom_read_box_rest( bs, box );
    mfro->length = lsmash_bs_get_be32( bs );
    box->size = lsmash_bs_get_pos( bs );
    isom_box_common_copy( mfro, box );
    return isom_add_print_func( root, mfro, level );
}

static int isom_read_box( lsmash_root_t *root, isom_box_t *box, isom_box_t *parent, uint64_t parent_pos, int level )
{
    lsmash_bs_t *bs = root->bs;
    memset( box, 0, sizeof(isom_box_t) );
    assert( parent && parent->root );
    box->root = parent->root;
    box->parent = parent;
    if( parent->size < parent_pos + ISOM_DEFAULT_BOX_HEADER_SIZE )
    {
        /* skip extra bytes */
        uint64_t rest_size = parent->size - parent_pos;
        lsmash_fseek( bs->stream, rest_size, SEEK_CUR );
        box->size = rest_size;
        return 0;
    }
    lsmash_bs_empty( bs );
    int ret = -1;
    if( !!(ret = isom_bs_read_box_common( bs, box )) )
        return ret;     /* return if reached EOF */
    ++level;
    if( parent->type == ISOM_BOX_TYPE_STSD )
        switch( box->type )
        {
            case ISOM_CODEC_TYPE_AVC1_VIDEO :
            case ISOM_CODEC_TYPE_AVC2_VIDEO :
            case ISOM_CODEC_TYPE_AVCP_VIDEO :
            case ISOM_CODEC_TYPE_DRAC_VIDEO :
            case ISOM_CODEC_TYPE_ENCV_VIDEO :
            case ISOM_CODEC_TYPE_MJP2_VIDEO :
            case ISOM_CODEC_TYPE_MP4V_VIDEO :
            case ISOM_CODEC_TYPE_MVC1_VIDEO :
            case ISOM_CODEC_TYPE_MVC2_VIDEO :
            case ISOM_CODEC_TYPE_S263_VIDEO :
            case ISOM_CODEC_TYPE_SVC1_VIDEO :
            case ISOM_CODEC_TYPE_VC_1_VIDEO :
                return isom_read_visual_description( root, box, parent, level );
            case ISOM_CODEC_TYPE_AC_3_AUDIO :
            case ISOM_CODEC_TYPE_ALAC_AUDIO :
            case ISOM_CODEC_TYPE_DRA1_AUDIO :
            case ISOM_CODEC_TYPE_DTSC_AUDIO :
            case ISOM_CODEC_TYPE_DTSH_AUDIO :
            case ISOM_CODEC_TYPE_DTSL_AUDIO :
            case ISOM_CODEC_TYPE_DTSE_AUDIO :
            case ISOM_CODEC_TYPE_EC_3_AUDIO :
            case ISOM_CODEC_TYPE_ENCA_AUDIO :
            case ISOM_CODEC_TYPE_G719_AUDIO :
            case ISOM_CODEC_TYPE_G726_AUDIO :
            case ISOM_CODEC_TYPE_M4AE_AUDIO :
            case ISOM_CODEC_TYPE_MLPA_AUDIO :
            case ISOM_CODEC_TYPE_MP4A_AUDIO :
            //case ISOM_CODEC_TYPE_RAW_AUDIO  :
            case ISOM_CODEC_TYPE_SAMR_AUDIO :
            case ISOM_CODEC_TYPE_SAWB_AUDIO :
            case ISOM_CODEC_TYPE_SAWP_AUDIO :
            case ISOM_CODEC_TYPE_SEVC_AUDIO :
            case ISOM_CODEC_TYPE_SQCP_AUDIO :
            case ISOM_CODEC_TYPE_SSMV_AUDIO :
            //case ISOM_CODEC_TYPE_TWOS_AUDIO :
            case QT_CODEC_TYPE_23NI_AUDIO :
            case QT_CODEC_TYPE_MAC3_AUDIO :
            case QT_CODEC_TYPE_MAC6_AUDIO :
            case QT_CODEC_TYPE_NONE_AUDIO :
            case QT_CODEC_TYPE_QDM2_AUDIO :
            case QT_CODEC_TYPE_QDMC_AUDIO :
            case QT_CODEC_TYPE_QCLP_AUDIO :
            case QT_CODEC_TYPE_AGSM_AUDIO :
            case QT_CODEC_TYPE_ALAW_AUDIO :
            case QT_CODEC_TYPE_CDX2_AUDIO :
            case QT_CODEC_TYPE_CDX4_AUDIO :
            case QT_CODEC_TYPE_DVCA_AUDIO :
            case QT_CODEC_TYPE_DVI_AUDIO  :
            case QT_CODEC_TYPE_FL32_AUDIO :
            case QT_CODEC_TYPE_FL64_AUDIO :
            case QT_CODEC_TYPE_IMA4_AUDIO :
            case QT_CODEC_TYPE_IN24_AUDIO :
            case QT_CODEC_TYPE_IN32_AUDIO :
            case QT_CODEC_TYPE_LPCM_AUDIO :
            case QT_CODEC_TYPE_RAW_AUDIO :
            case QT_CODEC_TYPE_SOWT_AUDIO :
            case QT_CODEC_TYPE_TWOS_AUDIO :
            case QT_CODEC_TYPE_ULAW_AUDIO :
            case QT_CODEC_TYPE_VDVA_AUDIO :
            case QT_CODEC_TYPE_FULLMP3_AUDIO :
            case QT_CODEC_TYPE_MP3_AUDIO :
            case QT_CODEC_TYPE_ADPCM2_AUDIO :
            case QT_CODEC_TYPE_ADPCM17_AUDIO :
            case QT_CODEC_TYPE_GSM49_AUDIO :
            case QT_CODEC_TYPE_NOT_SPECIFIED :
                return isom_read_audio_description( root, box, parent, level );
            case QT_CODEC_TYPE_TEXT_TEXT :
                return isom_read_text_description( root, box, parent, level );
            case ISOM_CODEC_TYPE_TX3G_TEXT :
                return isom_read_tx3g_description( root, box, parent, level );
            default :
                return isom_read_unknown_box( root, box, parent, level );
        }
    if( parent->type == QT_BOX_TYPE_WAVE )
        switch( box->type )
        {
            case QT_BOX_TYPE_FRMA :
                return isom_read_frma( root, box, parent, level );
            case QT_BOX_TYPE_ENDA :
                return isom_read_enda( root, box, parent, level );
            case ISOM_BOX_TYPE_ESDS :
                return isom_read_esds( root, box, parent, level );
            case QT_BOX_TYPE_TERMINATOR :
                return isom_read_terminator( root, box, parent, level );
            default :
                return isom_read_audio_specific( root, box, parent, level );
        }
    if( parent->type == ISOM_BOX_TYPE_TREF )
        return isom_read_track_reference_type( root, box, parent, level );
    switch( box->type )
    {
        case ISOM_BOX_TYPE_FTYP :
            return isom_read_ftyp( root, box, parent, level );
        case ISOM_BOX_TYPE_MOOV :
            return isom_read_moov( root, box, parent, level );
        case ISOM_BOX_TYPE_MVHD :
            return isom_read_mvhd( root, box, parent, level );
        case ISOM_BOX_TYPE_IODS :
            return isom_read_iods( root, box, parent, level );
        case ISOM_BOX_TYPE_ESDS :
            return isom_read_esds( root, box, parent, level );
        case ISOM_BOX_TYPE_TRAK :
            return isom_read_trak( root, box, parent, level );
        case ISOM_BOX_TYPE_TKHD :
            return isom_read_tkhd( root, box, parent, level );
        case QT_BOX_TYPE_TAPT :
            return isom_read_tapt( root, box, parent, level );
        case QT_BOX_TYPE_CLEF :
            return isom_read_clef( root, box, parent, level );
        case QT_BOX_TYPE_PROF :
            return isom_read_prof( root, box, parent, level );
        case QT_BOX_TYPE_ENOF :
            return isom_read_enof( root, box, parent, level );
        case ISOM_BOX_TYPE_EDTS :
            return isom_read_edts( root, box, parent, level );
        case ISOM_BOX_TYPE_ELST :
            return isom_read_elst( root, box, parent, level );
        case ISOM_BOX_TYPE_TREF :
            return isom_read_tref( root, box, parent, level );
        case ISOM_BOX_TYPE_MDIA :
            return isom_read_mdia( root, box, parent, level );
        case ISOM_BOX_TYPE_MDHD :
            return isom_read_mdhd( root, box, parent, level );
        case ISOM_BOX_TYPE_HDLR :
            return isom_read_hdlr( root, box, parent, level );
        case ISOM_BOX_TYPE_MINF :
            return isom_read_minf( root, box, parent, level );
        case ISOM_BOX_TYPE_VMHD :
            return isom_read_vmhd( root, box, parent, level );
        case ISOM_BOX_TYPE_SMHD :
            return isom_read_smhd( root, box, parent, level );
        case ISOM_BOX_TYPE_HMHD :
            return isom_read_hmhd( root, box, parent, level );
        case ISOM_BOX_TYPE_NMHD :
            return isom_read_nmhd( root, box, parent, level );
        case QT_BOX_TYPE_GMHD :
            return isom_read_gmhd( root, box, parent, level );
        case QT_BOX_TYPE_GMIN :
            return isom_read_gmin( root, box, parent, level );
        case QT_BOX_TYPE_TEXT :
            return isom_read_text( root, box, parent, level );
        case ISOM_BOX_TYPE_DINF :
            return isom_read_dinf( root, box, parent, level );
        case ISOM_BOX_TYPE_DREF :
            return isom_read_dref( root, box, parent, level );
        case ISOM_BOX_TYPE_URL  :
            return isom_read_url ( root, box, parent, level );
        case ISOM_BOX_TYPE_STBL :
            return isom_read_stbl( root, box, parent, level );
        case ISOM_BOX_TYPE_STSD :
            return isom_read_stsd( root, box, parent, level );
        case ISOM_BOX_TYPE_BTRT :
            return isom_read_btrt( root, box, parent, level );
        case ISOM_BOX_TYPE_CLAP :
            return isom_read_clap( root, box, parent, level );
        case ISOM_BOX_TYPE_PASP :
            return isom_read_pasp( root, box, parent, level );
        case QT_BOX_TYPE_COLR :
            return isom_read_colr( root, box, parent, level );
        case ISOM_BOX_TYPE_STSL :
            return isom_read_stsl( root, box, parent, level );
        case ISOM_BOX_TYPE_AVCC :
            return isom_read_avcC( root, box, parent, level );
        case QT_BOX_TYPE_WAVE :
            return isom_read_wave( root, box, parent, level );
        case QT_BOX_TYPE_CHAN :
            return isom_read_chan( root, box, parent, level );
        case ISOM_BOX_TYPE_FTAB :
            return isom_read_ftab( root, box, parent, level );
        case ISOM_BOX_TYPE_STTS :
            return isom_read_stts( root, box, parent, level );
        case ISOM_BOX_TYPE_CTTS :
            return isom_read_ctts( root, box, parent, level );
        case ISOM_BOX_TYPE_CSLG :
            return isom_read_cslg( root, box, parent, level );
        case ISOM_BOX_TYPE_STSS :
            return isom_read_stss( root, box, parent, level );
        case QT_BOX_TYPE_STPS :
            return isom_read_stps( root, box, parent, level );
        case ISOM_BOX_TYPE_SDTP :
            return isom_read_sdtp( root, box, parent, level );
        case ISOM_BOX_TYPE_STSC :
            return isom_read_stsc( root, box, parent, level );
        case ISOM_BOX_TYPE_STSZ :
            return isom_read_stsz( root, box, parent, level );
        case ISOM_BOX_TYPE_STCO :
        case ISOM_BOX_TYPE_CO64 :
            return isom_read_stco( root, box, parent, level );
        case ISOM_BOX_TYPE_SGPD :
            return isom_read_sgpd( root, box, parent, level );
        case ISOM_BOX_TYPE_SBGP :
            return isom_read_sbgp( root, box, parent, level );
        case ISOM_BOX_TYPE_UDTA :
            return isom_read_udta( root, box, parent, level );
        case ISOM_BOX_TYPE_CHPL :
            return isom_read_chpl( root, box, parent, level );
        case ISOM_BOX_TYPE_MVEX :
            return isom_read_mvex( root, box, parent, level );
        case ISOM_BOX_TYPE_MEHD :
            return isom_read_mehd( root, box, parent, level );
        case ISOM_BOX_TYPE_TREX :
            return isom_read_trex( root, box, parent, level );
        case ISOM_BOX_TYPE_MOOF :
            return isom_read_moof( root, box, parent, level );
        case ISOM_BOX_TYPE_MFHD :
            return isom_read_mfhd( root, box, parent, level );
        case ISOM_BOX_TYPE_TRAF :
            return isom_read_traf( root, box, parent, level );
        case ISOM_BOX_TYPE_TFHD :
            return isom_read_tfhd( root, box, parent, level );
        case ISOM_BOX_TYPE_TRUN :
            return isom_read_trun( root, box, parent, level );
        case ISOM_BOX_TYPE_FREE :
        case ISOM_BOX_TYPE_SKIP :
            return isom_read_free( root, box, parent, level );
        case ISOM_BOX_TYPE_MDAT :
            return isom_read_mdat( root, box, parent, level );
        case ISOM_BOX_TYPE_MFRA :
            return isom_read_mfra( root, box, parent, level );
        case ISOM_BOX_TYPE_TFRA :
            return isom_read_tfra( root, box, parent, level );
        case ISOM_BOX_TYPE_MFRO :
            return isom_read_mfro( root, box, parent, level );
        default :
            return isom_read_unknown_box( root, box, parent, level );
    }
}

int isom_read_root( lsmash_root_t *root )
{
    lsmash_bs_t *bs = root->bs;
    if( !bs )
        return -1;
    isom_box_t box;
    if( root->flags & LSMASH_FILE_MODE_DUMP )
    {
        root->print = lsmash_create_entry_list();
        if( !root->print )
            return -1;
    }
    root->size = UINT64_MAX;
    int ret = isom_read_children( root, &box, root, 0 );
    root->size = box.size;
    lsmash_bs_empty( bs );
    if( ret < 0 )
        return ret;
    return isom_check_compatibility( root );
}

#endif /* LSMASH_DEMUXER_ENABLED */