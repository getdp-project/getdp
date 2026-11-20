// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#include <string.h>
#include <algorithm>
#include <map>
#include <vector>
#include "GeoData.h"
#include "ProData.h"
#include "GeoEntity.h"
#include "GeoEntityTable.h"
#include "DofData.h"
#include "ListUtils.h"
#include "MallocUtils.h"
#include "Message.h"
#include "GetDPConfig.h"

#if defined(HAVE_MPI)
#include <mpi.h>
#endif

extern FILE *File_PRE;
extern struct GeoData *CurrentGeoData;
extern int Flag_XDATA;

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t N o d e s _ u v w                                         */
/* ------------------------------------------------------------------------ */

double *Geo_GetNodes_uvw(int Type, int *nbn)
{
  switch(Type) {
  case POINT_ELEMENT: *nbn = NbrNodes_Point; return (*Nodes_Point);

  case LINE: *nbn = NbrNodes_Line; return (*Nodes_Line);
  case TRIANGLE: *nbn = NbrNodes_Triangle; return (*Nodes_Triangle);
  case QUADRANGLE: *nbn = NbrNodes_Quadrangle; return (*Nodes_Quadrangle);
  case TETRAHEDRON: *nbn = NbrNodes_Tetrahedron; return (*Nodes_Tetrahedron);
  case HEXAHEDRON: *nbn = NbrNodes_Hexahedron; return (*Nodes_Hexahedron);
  case PRISM: *nbn = NbrNodes_Prism; return (*Nodes_Prism);
  case PYRAMID: *nbn = NbrNodes_Pyramid; return (*Nodes_Pyramid);

  case LINE_2: *nbn = NbrNodes_Line_2; return (*Nodes_Line_2);
  case TRIANGLE_2: *nbn = NbrNodes_Triangle_2; return (*Nodes_Triangle_2);
  case QUADRANGLE_2: *nbn = NbrNodes_Quadrangle_2; return (*Nodes_Quadrangle_2);
  case QUADRANGLE_2_8N:
    *nbn = NbrNodes_Quadrangle_2_8N;
    return (*Nodes_Quadrangle_2_8N);
  case TETRAHEDRON_2:
    *nbn = NbrNodes_Tetrahedron_2;
    return (*Nodes_Tetrahedron_2);

  default:
    Message::Error("Unknown type of Element in Geo_GetNodes_uvw");
    return (NULL);
  }
}

/* ------------------------------------------------------------------------ */
/*  G e o _ C r e a t e E d g e s O f E l e m e n t                         */
/* ------------------------------------------------------------------------ */

void Geo_CreateEdgesOfElement(struct Geo_Element *Geo_Element)
{
  int Nbr_Entities2, *D_Element;

  D_Element = Geo_GetIM_Den(Geo_Element->Type, &Nbr_Entities2);
  if(!Nbr_Entities2) return;
  Geo_CreateEntitiesOfElement(
    Nbr_Entities2, D_Element, Geo_Element->NbrNodes, Geo_Element->NumNodes,
    &Geo_Element->NbrEdges, &Geo_Element->NumEdges,
    &CurrentGeoData->NbrElementsWithEdges, &CurrentGeoData->NumCurrentEdge,
    CurrentGeoData->EdgesXNodes);
}

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t N o d e s O f E d g e I n E l e m e n t                   */
/* ------------------------------------------------------------------------ */

int *Geo_GetNodesOfEdgeInElement(struct Geo_Element *Geo_Element, int Num_Edge)
{
  int Nbr_Entities2;

  return (Geo_GetIM_Den(Geo_Element->Type, &Nbr_Entities2) +
          Num_Edge * NBR_MAX_SUBENTITIES_IN_ELEMENT);
}

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t N o d e s O f F a c e t I n E l e m e n t                 */
/* ------------------------------------------------------------------------ */

int *Geo_GetNodesOfFacetInElement(struct Geo_Element *Geo_Element,
                                  int Num_Facet)
{
  int Nbr_Entities2;

  return (Geo_GetIM_Dfn(Geo_Element->Type, &Nbr_Entities2) +
          Num_Facet * NBR_MAX_SUBENTITIES_IN_ELEMENT);
}

/* ------------------------------------------------------------------------ */
/*  G e o _ C r e a t e F a c e t s O f E l e m e n t                       */
/* ------------------------------------------------------------------------ */

void Geo_CreateFacetsOfElement(struct Geo_Element *Geo_Element)
{
  int Nbr_Entities2, *D_Element;

  D_Element = Geo_GetIM_Dfe(Geo_Element->Type, &Nbr_Entities2);
  if(!Nbr_Entities2) return;
  Geo_CreateEntitiesOfElement(
    Nbr_Entities2, D_Element, Geo_Element->NbrEdges, Geo_Element->NumEdges,
    &Geo_Element->NbrFacets, &Geo_Element->NumFacets,
    &CurrentGeoData->NbrElementsWithFacets, &CurrentGeoData->NumCurrentFacet,
    CurrentGeoData->FacetsXEdges);
}

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t I M _ D e n                                               */
/* ------------------------------------------------------------------------ */

int *Geo_GetIM_Den(int Type_Element, int *Nbe)
{
  switch(Type_Element) {
  case POINT_ELEMENT: *Nbe = NbrEdges_Point; return (NULL);

  case LINE:
  case LINE_2:
  case LINE_3:
  case LINE_4: *Nbe = NbrEdges_Line; return (*Den_Line);

  case TRIANGLE:
  case TRIANGLE_2:
  case TRIANGLE_3:
  case TRIANGLE_4: *Nbe = NbrEdges_Triangle; return (*Den_Triangle);

  case QUADRANGLE:
  case QUADRANGLE_2:
  case QUADRANGLE_2_8N:
  case QUADRANGLE_3:
  case QUADRANGLE_4: *Nbe = NbrEdges_Quadrangle; return (*Den_Quadrangle);

  case TETRAHEDRON:
  case TETRAHEDRON_2:
  case TETRAHEDRON_3:
  case TETRAHEDRON_4: *Nbe = NbrEdges_Tetrahedron; return (*Den_Tetrahedron);

  case HEXAHEDRON:
  case HEXAHEDRON_2:
  case HEXAHEDRON_2_20N:
  case HEXAHEDRON_3:
  case HEXAHEDRON_4: *Nbe = NbrEdges_Hexahedron; return (*Den_Hexahedron);

  case PRISM:
  case PRISM_2:
  case PRISM_2_15N:
  case PRISM_3:
  case PRISM_4: *Nbe = NbrEdges_Prism; return (*Den_Prism);

  case PYRAMID:
  case PYRAMID_2:
  case PYRAMID_2_13N:
  case PYRAMID_3: // case PYRAMID_4
    *Nbe = NbrEdges_Pyramid;
    return (*Den_Pyramid);

  default:
    Message::Error("Unknown incidence matrix for element type %d",
                   Type_Element);
    return (NULL);
  }
}

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t I M _ D f e                                               */
/* ------------------------------------------------------------------------ */

int *Geo_GetIM_Dfe(int Type_Element, int *Nbf)
{
  switch(Type_Element) {
  case POINT_ELEMENT: *Nbf = NbrFacets_Point; return (NULL);

  case LINE:
  case LINE_2:
  case LINE_3:
  case LINE_4: *Nbf = NbrFacets_Line; return (NULL);

  case TRIANGLE:
  case TRIANGLE_2:
  case TRIANGLE_3:
  case TRIANGLE_4: *Nbf = NbrFacets_Triangle; return (*Dfe_Triangle);

  case QUADRANGLE:
  case QUADRANGLE_2:
  case QUADRANGLE_2_8N:
  case QUADRANGLE_3:
  case QUADRANGLE_4: *Nbf = NbrFacets_Quadrangle; return (*Dfe_Quadrangle);

  case TETRAHEDRON:
  case TETRAHEDRON_2:
  case TETRAHEDRON_3:
  case TETRAHEDRON_4: *Nbf = NbrFacets_Tetrahedron; return (*Dfe_Tetrahedron);

  case HEXAHEDRON:
  case HEXAHEDRON_2:
  case HEXAHEDRON_2_20N:
  case HEXAHEDRON_3:
  case HEXAHEDRON_4: *Nbf = NbrFacets_Hexahedron; return (*Dfe_Hexahedron);

  case PRISM:
  case PRISM_2:
  case PRISM_2_15N:
  case PRISM_3:
  case PRISM_4: *Nbf = NbrFacets_Prism; return (*Dfe_Prism);

  case PYRAMID:
  case PYRAMID_2:
  case PYRAMID_2_13N:
  case PYRAMID_3: // case PYRAMID_4
    *Nbf = NbrFacets_Pyramid;
    return (*Dfe_Pyramid);

  default:
    Message::Error("Unknown incidence matrix for element type %d",
                   Type_Element);
    return (NULL);
  }
}

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t I M _ D f n                                               */
/* ------------------------------------------------------------------------ */

int *Geo_GetIM_Dfn(int Type_Element, int *Nbf)
{
  switch(Type_Element) {
  case POINT_ELEMENT: *Nbf = NbrFacets_Point; return (NULL);

  case LINE:
  case LINE_2:
  case LINE_3:
  case LINE_4: *Nbf = NbrFacets_Line; return (NULL);

  case TRIANGLE:
  case TRIANGLE_2:
  case TRIANGLE_3:
  case TRIANGLE_4: *Nbf = NbrFacets_Triangle; return (*Dfn_Triangle);

  case QUADRANGLE:
  case QUADRANGLE_2:
  case QUADRANGLE_2_8N:
  case QUADRANGLE_3:
  case QUADRANGLE_4: *Nbf = NbrFacets_Quadrangle; return (*Dfn_Quadrangle);

  case TETRAHEDRON:
  case TETRAHEDRON_2:
  case TETRAHEDRON_3:
  case TETRAHEDRON_4: *Nbf = NbrFacets_Tetrahedron; return (*Dfn_Tetrahedron);

  case HEXAHEDRON:
  case HEXAHEDRON_2:
  case HEXAHEDRON_2_20N:
  case HEXAHEDRON_3:
  case HEXAHEDRON_4: *Nbf = NbrFacets_Hexahedron; return (*Dfn_Hexahedron);

  case PRISM:
  case PRISM_2:
  case PRISM_2_15N:
  case PRISM_3:
  case PRISM_4: *Nbf = NbrFacets_Prism; return (*Dfn_Prism);

  case PYRAMID:
  case PYRAMID_2:
  case PYRAMID_2_13N:
  case PYRAMID_3: // case PYRAMID_4
    *Nbf = NbrFacets_Pyramid;
    return (*Dfn_Pyramid);

  default:
    Message::Error("Unknown incidence matrix for element type %d",
                   Type_Element);
    return (NULL);
  }
}

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t I M _ D e n _ X p                                         */
/* ------------------------------------------------------------------------ */

int *Geo_GetIM_Den_Xp(int Type_Element, int *Nbe, int *Nbn)
{
  switch(Type_Element) {
  case POINT_ELEMENT:
    *Nbe = NbrEdges_Point;
    *Nbn = NbrNodes_Point;
    return (NULL);

  case LINE:
  case LINE_2:
  case LINE_3:
  case LINE_4:
    *Nbe = NbrEdges_Line;
    *Nbn = NbrNodes_Line;
    return (Den_Line_Xp);

  case TRIANGLE:
  case TRIANGLE_2:
  case TRIANGLE_3:
  case TRIANGLE_4:
    *Nbe = NbrEdges_Triangle;
    *Nbn = NbrNodes_Triangle;
    return (Den_Triangle_Xp);

  case QUADRANGLE:
  case QUADRANGLE_2:
  case QUADRANGLE_2_8N:
  case QUADRANGLE_3:
  case QUADRANGLE_4:
    *Nbe = NbrEdges_Quadrangle;
    *Nbn = NbrNodes_Quadrangle;
    return (Den_Quadrangle_Xp);

  case TETRAHEDRON:
  case TETRAHEDRON_2:
  case TETRAHEDRON_3:
  case TETRAHEDRON_4:
    *Nbe = NbrEdges_Tetrahedron;
    *Nbn = NbrNodes_Tetrahedron;
    return (Den_Tetrahedron_Xp);

  case HEXAHEDRON:
  case HEXAHEDRON_2:
  case HEXAHEDRON_2_20N:
  case HEXAHEDRON_3:
  case HEXAHEDRON_4:
    *Nbe = NbrEdges_Hexahedron;
    *Nbn = NbrNodes_Hexahedron;
    return (Den_Hexahedron_Xp);

  case PRISM:
  case PRISM_2:
  case PRISM_2_15N:
  case PRISM_3:
  case PRISM_4:
    *Nbe = NbrEdges_Prism;
    *Nbn = NbrNodes_Prism;
    return (Den_Prism_Xp);

  case PYRAMID:
  case PYRAMID_2:
  case PYRAMID_2_13N:
  case PYRAMID_3: // case PYRAMID_4
    *Nbe = NbrEdges_Pyramid;
    *Nbn = NbrNodes_Pyramid;
    return (Den_Pyramid_Xp);

  default:
    Message::Error("Unknown incidence matrix for element type %d",
                   Type_Element);
    return (NULL);
  }
}

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t I M _ D f e _ X p                                         */
/* ------------------------------------------------------------------------ */

int *Geo_GetIM_Dfe_Xp(int Type_Element, int *nbf, int *nbe)
{
  switch(Type_Element) {
  case POINT_ELEMENT:
    *nbf = NbrFacets_Point;
    *nbe = NbrEdges_Point;
    return (NULL);

  case LINE:
  case LINE_2:
  case LINE_3:
  case LINE_4:
    *nbf = NbrFacets_Line;
    *nbe = NbrEdges_Line;
    return (NULL);

  case TRIANGLE:
  case TRIANGLE_2:
  case TRIANGLE_3:
  case TRIANGLE_4:
    *nbf = NbrFacets_Triangle;
    *nbe = NbrEdges_Triangle;
    return (Dfe_Triangle_Xp);

  case QUADRANGLE:
  case QUADRANGLE_2:
  case QUADRANGLE_2_8N:
  case QUADRANGLE_3:
  case QUADRANGLE_4:
    *nbf = NbrFacets_Quadrangle;
    *nbe = NbrEdges_Quadrangle;
    return (Dfe_Quadrangle_Xp);

  case TETRAHEDRON:
  case TETRAHEDRON_2:
  case TETRAHEDRON_3:
  case TETRAHEDRON_4:
    *nbf = NbrFacets_Tetrahedron;
    *nbe = NbrEdges_Tetrahedron;
    return (Dfe_Tetrahedron_Xp);

  case HEXAHEDRON:
  case HEXAHEDRON_2:
  case HEXAHEDRON_2_20N:
  case HEXAHEDRON_3:
  case HEXAHEDRON_4:
    *nbf = NbrFacets_Hexahedron;
    *nbe = NbrEdges_Hexahedron;
    return (Dfe_Hexahedron_Xp);

  case PRISM:
  case PRISM_2:
  case PRISM_2_15N:
  case PRISM_3:
  case PRISM_4:
    *nbf = NbrFacets_Prism;
    *nbe = NbrEdges_Prism;
    return (Dfe_Prism_Xp);

  case PYRAMID:
  case PYRAMID_2:
  case PYRAMID_2_13N:
  case PYRAMID_3: // case PYRAMID_4
    *nbf = NbrFacets_Pyramid;
    *nbe = NbrEdges_Pyramid;
    return (Dfe_Pyramid_Xp);

  default:
    Message::Error("Unknown incidence matrix for element type %d",
                   Type_Element);
    return (NULL);
  }
}

/* ------------------------------------------------------------------------ */
/*  G e o _ C r e a t e E n t i t i e s O f E l e m e n t                   */
/* ------------------------------------------------------------------------ */

void Geo_CreateEntitiesOfElement(
  int Nbr_Entities2, int *D_Element, int Geo_Element_NbrEntities1,
  int *Geo_Element_NumEntities1, int *Geo_Element_NbrEntities2,
  int **Geo_Element_NumEntities2, int *Geo_NbrElementsWithEntities2,
  int *Geo_NumCurrentEntity2, GeoEntityTable *Geo_Entities2XEntities1)
{
  int i, j, Nbr_Entities1, Num_Entities1[NBR_MAX_SUBENTITIES_IN_ELEMENT],
    Sign_Entity2;
  int *Entity_P, Entity;

  *Geo_Element_NbrEntities2 = Nbr_Entities2;
  *Geo_Element_NumEntities2 = (int *)Malloc(Nbr_Entities2 * sizeof(int));
  (*Geo_NbrElementsWithEntities2)++;

  for(i = 0; i < Nbr_Entities2; i++) {
    Entity_P = D_Element + i * NBR_MAX_SUBENTITIES_IN_ELEMENT;
    Nbr_Entities1 = 0;
    while((Entity = *(Entity_P++)))
      Num_Entities1[Nbr_Entities1++] = (Entity > 0) ?
                                         Geo_Element_NumEntities1[Entity - 1] :
                                         -Geo_Element_NumEntities1[-Entity - 1];

    qsort(Num_Entities1, Nbr_Entities1, sizeof(int), fcmp_absint);

    if(Num_Entities1[0] < 0) {
      Sign_Entity2 = -1;
      for(j = 0; j < Nbr_Entities1; j++) Num_Entities1[j] *= -1;
    }
    else
      Sign_Entity2 = 1;

    bool created;
    struct Entity2XEntity1 &Entity2 =
      Geo_Entities2XEntities1->get(Nbr_Entities1, Num_Entities1, created);
    if(created) {
      // When the ranks only have a part of the mesh, an edge or facet
      // numbered here does not have the number the other ranks give it. This
      // is fine as long as the numbers are only used on this rank (e.g. the
      // facets, to find the elements on one side of a surface), i.e. when
      // they were not numbered over the ranks (Geo_CreateEdgesAndFacets);
      // otherwise all the elements already have theirs.
      int level =
        (Geo_Entities2XEntities1 == CurrentGeoData->EdgesXNodes) ? 1 : 2;
      if((CurrentGeoData->IsPart || CurrentGeoData->HasParts) &&
         CurrentGeoData->EntitiesLevel >= level)
        Message::Error("%s created on a part of the mesh: its number is not "
                       "the one of the other ranks (use -all-dofs)",
                       (level == 1) ? "Edge" : "Facet");
      Entity2.Num = ++(*Geo_NumCurrentEntity2);
    }
    (*Geo_Element_NumEntities2)[i] = Entity2.Num * Sign_Entity2;
  }
}

/* ------------------------------------------------------------------------ */
/*  G e o _ C r e a t e E d g e s A n d F a c e t s                         */
/* ------------------------------------------------------------------------ */

#if defined(HAVE_MPI)
// Sends a list of integers to each rank, and returns the list that each rank
// sent to this one.
static void Exchange(const std::vector<std::vector<int> > &send,
                     std::vector<std::vector<int> > &recv)
{
  int np = Message::GetCommSize();
  std::vector<int> scounts(np), sdispls(np), rcounts(np), rdispls(np);
  int stotal = 0, rtotal = 0;
  for(int r = 0; r < np; r++) {
    scounts[r] = (int)send[r].size();
    sdispls[r] = stotal;
    stotal += scounts[r];
  }
  std::vector<int> sbuf(stotal + 1);
  for(int r = 0; r < np; r++)
    std::copy(send[r].begin(), send[r].end(), sbuf.begin() + sdispls[r]);
  MPI_Alltoall(&scounts[0], 1, MPI_INT, &rcounts[0], 1, MPI_INT, MPI_COMM_WORLD);
  for(int r = 0; r < np; r++) {
    rdispls[r] = rtotal;
    rtotal += rcounts[r];
  }
  std::vector<int> rbuf(rtotal + 1);
  MPI_Alltoallv(&sbuf[0], &scounts[0], &sdispls[0], MPI_INT, &rbuf[0],
                &rcounts[0], &rdispls[0], MPI_INT, MPI_COMM_WORLD);
  recv.assign(np, std::vector<int>());
  for(int r = 0; r < np; r++)
    recv[r].assign(rbuf.begin() + rdispls[r],
                   rbuf.begin() + rdispls[r] + rcounts[r]);
}

// Gives the same numbers on all the ranks to the edges (or to the facets) that
// each rank has created, with numbers of its own, for the elements of its part
// of the mesh.
//
// An entity is identified by its list of nodes (or of edges), and is owned by
// the lowest of the ranks of the partitions of the elements that contain it.
// As a rank does not have all these elements, it does not know the owner: the
// ranks agree on it through a third rank, chosen from the list alone (a hash
// of it), to which each rank sends its entities with the lowest rank it sees.
// The owners then number their entities, one rank after the other, and the
// other ranks ask them for these numbers.
static void Geo_NumberEntitiesOverRanks(GeoEntityTable *table, bool facets,
                                        int *NumCurrentEntity)
{
  struct GeoData *geo = CurrentGeoData;
  const int np = Message::GetCommSize(), me = Message::GetCommRank();
  std::vector<struct Entity2XEntity1> &entities = table->entities();
  const int n = (int)entities.size();
  const int L = 4; // largest number of nodes of an edge, or of edges of a facet

  // the position of each entity from its own number, and the lowest rank of
  // the partitions of the elements of this rank that contain it
  std::vector<int> position(*NumCurrentEntity + 1, -1), lowest(n, np);
  for(int i = 0; i < n; i++) {
    if(entities[i].NbrEntities > L) {
      Message::Error("Edge or facet with %d nodes or edges",
                     entities[i].NbrEntities);
      return;
    }
    position[entities[i].Num] = i;
  }
  for(int e = 0; e < List_Nbr(geo->Elements); e++) {
    struct Geo_Element *ge = (struct Geo_Element *)List_Pointer(geo->Elements, e);
    int rank = (ge->Partition > 0) ? (ge->Partition - 1) % np : np - 1;
    int nbr = facets ? ge->NbrFacets : ge->NbrEdges;
    int *num = facets ? ge->NumFacets : ge->NumEdges;
    for(int k = 0; k < nbr; k++) {
      int i = position[abs(num[k])];
      lowest[i] = std::min(lowest[i], rank);
    }
  }

  // ask the owner of each entity to the rank given by its list
  auto key = [&](int i, int *list) {
    std::fill(list, list + L, 0);
    std::copy(entities[i].NumEntities,
              entities[i].NumEntities + entities[i].NbrEntities, list);
  };
  std::vector<std::vector<int> > send(np), recv, asked(np);
  for(int i = 0; i < n; i++) {
    int list[L];
    key(i, list);
    std::size_t h = 0;
    for(int k = 0; k < L; k++)
      h = (h ^ (std::size_t)(unsigned int)list[k]) * 0x9E3779B97F4A7C15ULL;
    int rank = (int)((h ^ (h >> 29)) % (std::size_t)np);
    send[rank].insert(send[rank].end(), list, list + L);
    send[rank].push_back(lowest[i]);
    asked[rank].push_back(i);
  }
  Exchange(send, recv);
  {
    std::map<std::vector<int>, int> owners;
    for(int r = 0; r < np; r++)
      for(std::size_t j = 0; j < recv[r].size(); j += L + 1) {
        std::vector<int> list(recv[r].begin() + j, recv[r].begin() + j + L);
        auto it = owners.find(list);
        if(it == owners.end())
          owners[list] = recv[r][j + L];
        else
          it->second = std::min(it->second, recv[r][j + L]);
      }
    for(int r = 0; r < np; r++) {
      send[r].clear();
      for(std::size_t j = 0; j < recv[r].size(); j += L + 1)
        send[r].push_back(owners[std::vector<int>(recv[r].begin() + j,
                                                  recv[r].begin() + j + L)]);
    }
  }
  Exchange(send, recv);
  std::vector<int> owner(n);
  for(int r = 0; r < np; r++)
    for(std::size_t j = 0; j < recv[r].size(); j++) owner[asked[r][j]] = recv[r][j];

  // number the entities owned by this rank, sorted by list, after those of
  // the previous ranks
  std::vector<int> owned;
  for(int i = 0; i < n; i++)
    if(owner[i] == me) owned.push_back(i);
  std::sort(owned.begin(), owned.end(), [&](int a, int b) {
    int la[L], lb[L];
    key(a, la);
    key(b, lb);
    return std::lexicographical_compare(la, la + L, lb, lb + L);
  });
  int count = (int)owned.size(), before = 0, total = 0;
  MPI_Scan(&count, &before, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&count, &total, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
  before -= count;
  std::vector<int> number(n, 0);
  for(int j = 0; j < count; j++) number[owned[j]] = before + j + 1;

  // ask the numbers of the other entities to their owners
  for(int r = 0; r < np; r++) {
    send[r].clear();
    asked[r].clear();
  }
  for(int i = 0; i < n; i++) {
    if(owner[i] == me) continue;
    int list[L];
    key(i, list);
    send[owner[i]].insert(send[owner[i]].end(), list, list + L);
    asked[owner[i]].push_back(i);
  }
  Exchange(send, recv);
  for(int r = 0; r < np; r++) {
    send[r].clear();
    for(std::size_t j = 0; j < recv[r].size(); j += L) {
      int nbr = 0;
      while(nbr < L && recv[r][j + nbr]) nbr++;
      bool created;
      struct Entity2XEntity1 &entity = table->get(nbr, &recv[r][j], created);
      int i = (int)(&entity - &entities[0]);
      if(created || number[i] == 0)
        Message::Error("Edge or facet of rank %d unknown on rank %d", r, me);
      send[r].push_back(created ? 0 : number[i]);
    }
  }
  Exchange(send, recv);
  for(int r = 0; r < np; r++)
    for(std::size_t j = 0; j < recv[r].size(); j++) number[asked[r][j]] = recv[r][j];

  // replace the numbers
  for(int e = 0; e < List_Nbr(geo->Elements); e++) {
    struct Geo_Element *ge = (struct Geo_Element *)List_Pointer(geo->Elements, e);
    int nbr = facets ? ge->NbrFacets : ge->NbrEdges;
    int *num = facets ? ge->NumFacets : ge->NumEdges;
    for(int k = 0; k < nbr; k++)
      num[k] = (num[k] > 0) ? number[position[num[k]]] : -number[position[-num[k]]];
  }
  for(int i = 0; i < n; i++) entities[i].Num = number[i];
  *NumCurrentEntity = total;
}
#endif

// Creates the edges (and the facets) of all the elements of the current mesh.
//
// The edges and facets are numbered in the order of their creation, and these
// numbers identify their Dofs: in parallel all the ranks must have the same
// ones. When the ranks only have a part of the mesh (Geo_SendParts), each one
// creates those of its elements, which are then numbered over the ranks; the
// facets, which are defined by their edges, after the edges.
void Geo_CreateEdgesAndFacets(bool facets)
{
  struct GeoData *geo = CurrentGeoData;
  int level = facets ? 2 : 1;
  if(geo->EntitiesLevel >= level) return;
  int n = List_Nbr(geo->Elements);
  struct Geo_Element *e0 =
    n ? (struct Geo_Element *)List_Pointer(geo->Elements, 0) : NULL;
  const bool parts = geo->HasParts || geo->IsPart;

  if(geo->EntitiesLevel < 1) {
    for(int e = 0; e < n; e++)
      if(e0[e].NbrEdges == 0) Geo_CreateEdgesOfElement(&e0[e]);
#if defined(HAVE_MPI)
    if(parts)
      Geo_NumberEntitiesOverRanks(geo->EdgesXNodes, false, &geo->NumCurrentEdge);
#endif
  }
  if(facets) {
    for(int e = 0; e < n; e++)
      if(e0[e].NbrFacets == 0) Geo_CreateFacetsOfElement(&e0[e]);
#if defined(HAVE_MPI)
    if(parts)
      Geo_NumberEntitiesOverRanks(geo->FacetsXEdges, true, &geo->NumCurrentFacet);
#endif
  }
  geo->EntitiesLevel = level;
}

/* ------------------------------------------------------------------------ */
/*  f c m p _ E 2 X E 1                                                     */
/* ------------------------------------------------------------------------ */

int fcmp_E2XE1(const void *a, const void *b)
{
  int i;

  if(((struct Entity2XEntity1 *)a)->NbrEntities !=
     ((struct Entity2XEntity1 *)b)->NbrEntities)
    return ((struct Entity2XEntity1 *)a)->NbrEntities -
           ((struct Entity2XEntity1 *)b)->NbrEntities;

  for(i = 0; i < ((struct Entity2XEntity1 *)a)->NbrEntities; i++) {
    if(((struct Entity2XEntity1 *)a)->NumEntities[i] >
       ((struct Entity2XEntity1 *)b)->NumEntities[i])
      return 1;
    if(((struct Entity2XEntity1 *)a)->NumEntities[i] <
       ((struct Entity2XEntity1 *)b)->NumEntities[i])
      return -1;
  }

  return 0;
}

/* ------------------------------------------------------------------------ */
/*  f r e e _ E 2 X E 1                                                     */
/* ------------------------------------------------------------------------ */

void free_E2XE1(void *a, void *b)
{
  Free(((struct Entity2XEntity1 *)a)->NumEntities);
}

/* ------------------------------------------------------------------------ */
/*  G e o _ W r i t e F i l e P R E                                         */
/* ------------------------------------------------------------------------ */

// The number of elements of the partition of this rank and the sum of their
// numbers, to check that a mesh is partitioned as another one.
static void Geo_CountElementsOfPartition(struct GeoData *GeoData_P,
                                         long long *count, long long *sum)
{
  *count = *sum = 0;
  for(int i = 0; i < List_Nbr(GeoData_P->Elements); i++) {
    struct Geo_Element *e =
      (struct Geo_Element *)List_Pointer(GeoData_P->Elements, i);
    if(!Dof_IsPartitionOfThisRank(e->Partition)) continue;
    (*count)++;
    *sum += e->Num;
  }
}

// The edges (what = 0) or the facets (1) of the elements of the mesh, which
// are identified by their number. A rank that only has a part of the mesh
// writes those of all its elements (those of its partition and their
// neighbours, which the section of another rank also has); the first rank, if
// it has the whole mesh, those of the elements of its partition.
static void Geo_PrintElementEntitiesPRE(struct GeoData *GeoData_P, int what,
                                        bool byRank)
{
  int n = List_Nbr(GeoData_P->Elements), count = 0;
  struct Geo_Element *e0 =
    n ? (struct Geo_Element *)List_Pointer(GeoData_P->Elements, 0) : NULL;
  auto nbr = [&](int i) {
    if(byRank && GeoData_P->HasParts &&
       !Dof_IsPartitionOfThisRank(e0[i].Partition))
      return 0;
    return what ? e0[i].NbrFacets : e0[i].NbrEdges;
  };
  for(int i = 0; i < n; i++)
    if(nbr(i)) count++;
  Dof_PrintfPRE(what ? "$ElementFacets\n" : "$ElementEdges\n");
  Dof_PrintfPRE("%d %d\n", GeoData_P->Num, count);
  for(int i = 0; i < n; i++) {
    if(!nbr(i)) continue;
    int *num = what ? e0[i].NumFacets : e0[i].NumEdges;
    Dof_PrintfPRE("%d %d", e0[i].Num, nbr(i));
    for(int j = 0; j < nbr(i); j++) Dof_PrintfPRE(" %d", num[j]);
    Dof_PrintfPRE("\n");
  }
  Dof_PrintfPRE(what ? "$EndElementFacets\n" : "$EndElementEdges\n");
}

void Geo_WriteFilePRE(struct GeoData *GeoData_P, List_T *Group_L)
{
  const bool byRank = GeoData_P->HasParts || GeoData_P->IsPart;
  const bool write = !(Message::GetIsCommWorld() && Message::GetCommRank());

  if(byRank || write) {
    if(byRank) {
      long long count, sum;
      Geo_CountElementsOfPartition(GeoData_P, &count, &sum);
      Dof_PrintfPRE("$ElementsOfPartition\n%d %lld %lld\n"
                    "$EndElementsOfPartition\n", GeoData_P->Num, count, sum);
    }
    if(GeoData_P->NumCurrentEdge)
      Geo_PrintElementEntitiesPRE(GeoData_P, 0, byRank);
    if(GeoData_P->NumCurrentFacet)
      Geo_PrintElementEntitiesPRE(GeoData_P, 1, byRank);
    Dof_WriteSectionPRE(byRank);
  }

  if(!write) return;

  int i, j, Index_Group, Nbr_Entities, *Num_Entities;
  struct Group *Group_P;

  if(Flag_XDATA && GeoData_P->NumCurrentEdge) {
    fprintf(File_PRE, "$EdgesXNodes  /* Never used, only for test */\n");
    fprintf(File_PRE, "%d %d\n", GeoData_P->Num,
            (int)GeoData_P->EdgesXNodes->size());
    for(auto e : GeoData_P->EdgesXNodes->sorted())
      Geo_WriteEntities2XEntities1(e, NULL);
    fprintf(File_PRE, "$EndEdgesXNodes\n");
  }
  if(Flag_XDATA && GeoData_P->NumCurrentFacet) {
    fprintf(File_PRE, "$FacetsXEdges  /* Never used, only for test */\n");
    fprintf(File_PRE, "%d %d\n", GeoData_P->Num,
            (int)GeoData_P->FacetsXEdges->size());
    for(auto e : GeoData_P->FacetsXEdges->sorted())
      Geo_WriteEntities2XEntities1(e, NULL);
    fprintf(File_PRE, "$EndFacetsXEdges\n");
  }

  /*  E x t e n d e d G r o u p  */

  if(GeoData_P->GroupForPRE != NULL) {
    for(i = 0; i < List_Nbr(GeoData_P->GroupForPRE); i++) {
      List_Read(GeoData_P->GroupForPRE, i, &Index_Group);
      Group_P = (struct Group *)List_Pointer(Group_L, Index_Group);

      fprintf(File_PRE, "$ExtendedGroup  /* %s */\n", Group_P->Name);
      fprintf(File_PRE, "%d %d\n", Index_Group,
              Nbr_Entities = List_Nbr(Group_P->ExtendedList));
      if(Nbr_Entities) {
        Num_Entities = (int *)List_Pointer(Group_P->ExtendedList, 0);
        for(j = 0; j < Nbr_Entities; j++) {
          fprintf(File_PRE, (j % 10) ? " %d" : "%d", Num_Entities[j]);
          if(j % 10 == 9) fprintf(File_PRE, "\n");
        }
        if(j % 10) fprintf(File_PRE, "\n");
        fprintf(File_PRE, "$EndExtendedGroup\n");
      }
    }
  }
}

/* --------------------------------------------------------- */
/*  G e o _ W r i t e E n t i t i e s 2 X E n t i t i e s 1  */
/* --------------------------------------------------------- */

void Geo_WriteEntities2XEntities1(void *a, void *b)
{
  int i;

  fprintf(File_PRE, "%d %d", ((struct Entity2XEntity1 *)a)->Num,
          ((struct Entity2XEntity1 *)a)->NbrEntities);
  for(i = 0; i < ((struct Entity2XEntity1 *)a)->NbrEntities; i++)
    fprintf(File_PRE, " %d", ((struct Entity2XEntity1 *)a)->NumEntities[i]);
  fprintf(File_PRE, "\n");
}

/* ------------------------------------------------------------------------ */
/*  G e o _ R e a d F i l e P R E                                           */
/* ------------------------------------------------------------------------ */

void Geo_ReadFilePRE(struct GeoData *GeoData_P0, int NbrGeoData,
                     List_T *Group_L)
{
  Message::Barrier();

  struct GeoData *GeoData_P;
  struct Geo_Element *Geo_Element_P0, *Geo_Element_P;
  struct Group *Group_P;
  int i, Index_Element, Nbr_Entities, j, Index_Group, Num_Entity;
  int GeoDataIndex;
  char String[256];

  for(GeoDataIndex = 0; GeoDataIndex < NbrGeoData; GeoDataIndex++) {
    if(!(GeoData_P0 + GeoDataIndex)->Elements) {
      Message::Warning("No Element in GeoData %d", GeoDataIndex);
      return;
    }
  }

  // a rank that only has its part of the mesh only reads its section
  const bool own = Dof_ReadsOwnSectionsPRE() && GeoData_P0->IsPart;

  while(1) {
    if(own) Dof_SkipOtherSectionsPRE();
    do {
      if(!fgets(String, sizeof(String), File_PRE) || feof(File_PRE)) break;
    } while(String[0] != '$');

    if(feof(File_PRE)) break;

    /*  E l e m e n t s O f P a r t i t i o n  */

    if(!strncmp(&String[1], "ElementsOfPartition", 19)) {
      long long count, sum, c, s;
      if(fscanf(File_PRE, "%d %lld %lld", &GeoDataIndex, &count, &sum) != 3 ||
         GeoDataIndex > NbrGeoData - 1) {
        Message::Error("Could not read the elements of the partition");
        return;
      }
      if(own) {
        Geo_CountElementsOfPartition(GeoData_P0 + GeoDataIndex, &c, &s);
        if(c != count || s != sum) {
          Message::Error("The mesh partition of rank %d is not the one of the "
                         "pre-processing (%lld elements instead of %lld): use "
                         "the same mesh files or -all-dofs",
                         Message::GetCommRank(), c, count);
          return;
        }
      }
    }

    /*  E l e m e n t E d g e s ,   E l e m e n t F a c e t s  */

    // the edges or the facets of elements given by their number (there can be
    // several such sections, see Geo_WriteFilePRE)
    else if(!strncmp(&String[1], "ElementEdges", 12) ||
            !strncmp(&String[1], "ElementFacets", 13)) {
      const bool facets = (String[8] == 'F');
      if(fscanf(File_PRE, "%d %d", &GeoDataIndex, &Nbr_Entities) != 2 ||
         GeoDataIndex > NbrGeoData - 1) {
        Message::Error("Could not read the edges or facets of the elements");
        return;
      }
      GeoData_P = GeoData_P0 + GeoDataIndex;
      const int count = Nbr_Entities;
      std::vector<int> num;
      for(i = 0; i < count; i++) {
        struct Geo_Element key;
        if(fscanf(File_PRE, "%d %d", &key.Num, &Nbr_Entities) != 2) {
          Message::Error("Could not read element and number of edges or facets");
          return;
        }
        num.resize(Nbr_Entities + 1);
        for(j = 0; j < Nbr_Entities; j++) {
          if(fscanf(File_PRE, "%d", &num[j]) != 1) {
            Message::Error("Could not read edge or facet");
            return;
          }
        }
        Geo_Element_P =
          (struct Geo_Element *)List_PQuery(GeoData_P->Elements, &key, fcmp_Elm);
        if(!Geo_Element_P) continue; // not in the part of the mesh of the rank
        int *&list = facets ? Geo_Element_P->NumFacets : Geo_Element_P->NumEdges;
        int &nbr = facets ? Geo_Element_P->NbrFacets : Geo_Element_P->NbrEdges;
        if(nbr) continue; // already read
        nbr = Nbr_Entities;
        list = (int *)Malloc(Nbr_Entities * sizeof(int));
        std::copy(num.begin(), num.begin() + Nbr_Entities, list);
        (facets ? GeoData_P->NbrElementsWithFacets :
                  GeoData_P->NbrElementsWithEdges)++;
      }
    }

    /*  E l e m e n t s X E d g e s  (elements given by their position, as */
    /*  written by the previous versions)                                  */

    else if(!strncmp(&String[1], "ElementsXEdges", 14)) {
      if(fscanf(File_PRE, "%d", &GeoDataIndex) != 1) {
        Message::Error("Could not read GeoData index");
        return;
      }
      if(GeoDataIndex > NbrGeoData - 1) {
        Message::Error("Unknown GeoData: %d", GeoDataIndex);
        return;
      }

      GeoData_P = GeoData_P0 + GeoDataIndex;
      Geo_Element_P0 =
        (struct Geo_Element *)List_Pointer(GeoData_P->Elements, 0);

      if(fscanf(File_PRE, "%d", &GeoData_P->NbrElementsWithEdges) != 1) {
        Message::Error("Could not read number of elements with edges");
        return;
      }
      for(i = 0; i < GeoData_P->NbrElementsWithEdges; i++) {
        if(fscanf(File_PRE, "%d %d", &Index_Element, &Nbr_Entities) != 2) {
          Message::Error("Could not read element index and number of edges");
          return;
        }
        Geo_Element_P = Geo_Element_P0 + Index_Element;
        Geo_Element_P->NbrEdges = Nbr_Entities;
        Geo_Element_P->NumEdges = (int *)Malloc(Nbr_Entities * sizeof(int));
        for(j = 0; j < Geo_Element_P->NbrEdges; j++) {
          if(fscanf(File_PRE, "%d", &Geo_Element_P->NumEdges[j]) != 1) {
            Message::Error("Could not read edge");
            return;
          }
        }
      }
    }

    /*  E l e m e n t s X F a c e t s  */

    else if(!strncmp(&String[1], "ElementsXFacets", 15)) {
      if(fscanf(File_PRE, "%d", &GeoDataIndex) != 1) {
        Message::Error("Could not read GeoData index");
        return;
      }
      if(GeoDataIndex > NbrGeoData - 1) {
        Message::Error("Unknown GeoData: %d", GeoDataIndex);
        return;
      }

      GeoData_P = GeoData_P0 + GeoDataIndex;
      Geo_Element_P0 =
        (struct Geo_Element *)List_Pointer(GeoData_P->Elements, 0);

      if(fscanf(File_PRE, "%d", &GeoData_P->NbrElementsWithFacets) != 1) {
        Message::Error("Could not read number of elements with facets");
        return;
      }
      for(i = 0; i < GeoData_P->NbrElementsWithFacets; i++) {
        if(fscanf(File_PRE, "%d %d", &Index_Element, &Nbr_Entities) != 2) {
          Message::Error("Could not read element index and number of facets");
          return;
        }
        Geo_Element_P = Geo_Element_P0 + Index_Element;
        Geo_Element_P->NbrFacets = Nbr_Entities;
        Geo_Element_P->NumFacets = (int *)Malloc(Nbr_Entities * sizeof(int));
        for(j = 0; j < Geo_Element_P->NbrFacets; j++) {
          if(fscanf(File_PRE, "%d", &Geo_Element_P->NumFacets[j]) != 1) {
            Message::Error("Could not read facet");
            return;
          }
        }
      }
    }

    /*  E x t e  n d e d G r o u p  */

    else if(!strncmp(&String[1], "ExtendedGroup", 13)) {
      if(fscanf(File_PRE, "%d %d", &Index_Group, &Nbr_Entities) != 2) {
        Message::Error(
          "Could not read extended group index and number of entities");
        return;
      }
      Group_P = (struct Group *)List_Pointer(Group_L, Index_Group);
      Group_P->ExtendedList = List_Create(Nbr_Entities, 1, sizeof(int));
      for(i = 0; i < Nbr_Entities; i++) {
        if(fscanf(File_PRE, "%d", &Num_Entity) != 1) {
          Message::Error("Could not read entity");
          return;
        }
        List_Add(Group_P->ExtendedList, &Num_Entity);
      }
    }

    do {
      if(!fgets(String, sizeof(String), File_PRE) || feof(File_PRE)) break;
    } while(String[0] != '$');

  } /* while 1 ... */
}

/* ------------------------------------------------------------------------ */
/*  G e o  _ A d d G r o u p F o r P R E                                    */
/* ------------------------------------------------------------------------ */

void Geo_AddGroupForPRE(int Num)
{
  if(CurrentGeoData->GroupForPRE == NULL)
    CurrentGeoData->GroupForPRE = List_Create(2, 2, sizeof(int));

  List_Add(CurrentGeoData->GroupForPRE, &Num);
}
