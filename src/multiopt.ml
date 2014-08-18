(*
 *
 * Copyright (c) 2012-2013, 
 *  Wes Weimer          <weimer@cs.virginia.edu>
 *  Stephanie Forrest   <forrest@cs.unm.edu>
 *  Claire Le Goues     <legoues@cs.virginia.edu>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * 3. The names of the contributors may not be used to endorse or promote
 * products derived from this software without specific prior written
 * permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *)
(** Multiopt provides multi-objective search strategies.  Currently implements
    NSGA-II, based on:
    http://www.mathworks.com/matlabcentral/fileexchange/10429-nsga-ii-a-multi-objective-optimization-algorithm*)

(* Note: CLG did not write this and has made minimal effort to understand it.  I
   did, however, reformat a lot to get everything closer to 80 characters per
   line; the use of the imperative style in this module combined with OCaml's
   standard indentation practices meant that basically everything was shifted
   really far to the right by the time we got to step 3.3 of the algorithm.  I
   thus do a lot of let _ = imperative computation in, trying to break up the
   computation into the steps suggested by the algorithm.  This is only in the
   interest of readability *)

open Printf
open Global
open Fitness
open Rep
open Population

let minimize = ref false 
let no_inf = ref false 
let num_objectives = ref 2  (* number of objectives *) 
let _ = 
  options := !options @ [
    "--multiopt-minimize", Arg.Set minimize, " minimize multiopt objective";

    "--multiopt-no-inf", Arg.Set no_inf, " avoid infinite values";

    "--num-objectives", Arg.Set_int num_objectives, "X expect X objective values";
  ] 

(* The implementation of NGSA-II below requires O(n^2) fitness lookups. The
   md5sum-based fitness cache from the Rep module is much faster than
   re-evaluating the variant's fitness, but is noticeably slow during replay.
   So we create our own cache here, using the rep name as a key.

   NOTE: this asusmes that name1 == name2 implies fitness1 == fitness2.
   *)
let yet_another_fitness_cache = Hashtbl.create 255

let evaluate (rep : ('a,'b) representation) = 
  if Hashtbl.mem yet_another_fitness_cache (rep#name ()) then
    Hashtbl.find yet_another_fitness_cache (rep#name ())
  else begin
    let _, values = rep#test_case (Single_Fitness) in 
    rep#cleanup () ;
    debug "\t%s\t%s\n" (String.concat " " (List.map (Printf.sprintf "%g") (Array.to_list values))) (rep#name());
    let values =
      if Array.length values < !num_objectives then begin
        if !minimize then 
          Array.make !num_objectives infinity 
        else 
          Array.make !num_objectives neg_infinity 
      end else values 
    in
    Hashtbl.add yet_another_fitness_cache (rep#name ()) values;
    values
  end

let is_pessimal arr = 
  if !minimize then 
    arr = Array.make !num_objectives infinity 
  else 
    arr = Array.make !num_objectives neg_infinity 

(* NGSA-II *)

let rephash_replace h x y = Hashtbl.replace h (x#name ()) (y) 
let rephash_add h x y = Hashtbl.add h (x#name ()) (y) 
let rephash_find h x = Hashtbl.find h (x#name ())  
let rephash_find_all h x = Hashtbl.find_all h (x#name ())  
let rephash_mem h x = Hashtbl.mem h (x#name ())  

let rec ngsa_ii (original : ('a,'b) Rep.representation) (incoming_pop) : unit =
  original#reduce_search_space (fun _ -> true) (not (!Search.promut <= 0));
  original#register_mutations 
    [(Delete_mut,!Search.del_prob); (Append_mut,!Search.app_prob); 
     (Swap_mut,!Search.swap_prob); (Replace_mut,!Search.rep_prob)];

  debug "multiopt: ngsa_ii begins (%d generations left)\n" !Search.generations;

  let current = ref incoming_pop in 
    for gen = 1 to !Search.generations do
      let _ = 
        debug "multiopt: ngsa_ii generation %d begins\n" gen 
      in
      let is_last_generation = gen = !Search.generations in 
      let next_generation = 
        ngsa_ii_internal original !current ~is_last_generation
      in 
        if !Rep.always_keep_source then begin
          (* If we don't keep the source, these generation files will be
             empty anyway... *)
          let filename = Printf.sprintf "generation-%04d.list" gen in 
            debug "multiopt: printing %s\n" filename ;
            let fout = open_out filename in 
              List.iter (fun var ->
                let names = var#source_name in
                let rec handle names = 
                  match names with
                  | [] -> ()
                  | [one] -> Printf.fprintf fout "%s\n" one
                  | first :: rest -> 
                    Printf.fprintf fout "%s," first ;
                    handle rest
                in
                  handle names ; 
              ) next_generation ;
              close_out fout ; 
        end ;
        current := next_generation 
    done ;
    debug "multiopt: ngsa_ii end\n" 

and ngsa_ii_internal 
    ?(is_last_generation=false) (original) incoming_pop =

  (* Step numbers follow Seshadri's paper *)
  (****** 3.1. Population Initialization ******)
  let pop = 
    match incoming_pop with
      [] -> begin
        debug "multiopt: generating initial population\n" ; 
        let pop = ref [original#copy ()] in (* our GP population *) 
          for i = 1 to pred !Population.popsize do
            (* initialize the population to a bunch of random mutants *) 
            pop := (Search.mutate original) :: !pop 
          done ;
          !pop
      end
    | _ -> 
      (debug "multiopt: using previous population\n" ; incoming_pop)
  in 

  let ngsa_ii_sort pop = begin
    let _ = 
      debug "multiopt: beginning sort\n" ; 
      Gc.compact()
    in

    let popa = Array.of_list pop in
    let popsize = Array.length popa in
    let fitness = Array.init popsize (fun i -> evaluate popa.(i)) in
    let names   = Array.init popsize (fun i -> popa.(i)#name ()) in

    let f_max = Array.make !num_objectives neg_infinity in
    let f_min = Array.make !num_objectives infinity in

    let _ =
      debug "multiopt: computing f_max and f_min %d \n"  (List.length pop)
    in

    let _ =
      Array.iteri (fun pi p ->
        let p_values = fitness.(pi) in 
          Array.iteri (fun m fval ->
            if m < !num_objectives then begin
              f_max.(m) <- max f_max.(m) fval ;
              f_min.(m) <- min f_min.(m) fval ;
            end ;
          ) p_values ;
      ) popa ; 
      for m = 0 to pred !num_objectives do
        debug "multiopt: %g <= objective %d <= %g\n" f_min.(m) m f_max.(m)
      done 
    in

    (****** 3.2. Non-Dominated Sort ******)
    let _ =
      debug "multiopt: first non-dominated sort begins\n" 
    in

    (* [nametbl] maps rep names to a canonical table index. We use this to get
       the index for an arbitrary rep, so that most of this algorithm can use
       array indexing rather than slower hashtable lookups (and key generation).

       This also means that distinct variants with the same name will map to
       the same index. The [ind] array maps the indices for reps to the index
       for the canonical rep with the same name. *)
    let nametbl = Hashtbl.create 255 in
    let ind = Array.make popsize (-1) in
    let _ =
      Array.iteri (fun i n ->
        if not (Hashtbl.mem nametbl n) then 
          Hashtbl.replace nametbl n i;
        ind.(i) <- Hashtbl.find nametbl n
      ) names
    in

    let dominates pi qi : bool =
      let p_values = fitness.(pi) in 
      let q_values = fitness.(qi) in 
        assert(Array.length p_values = Array.length q_values) ; 
        let better = ref 0 in
        let same = ref 0 in
        let worse = ref 0 in 
          for i = 0 to pred (Array.length p_values) do
            if p_values.(i) > q_values.(i) then 
              (if !minimize then incr worse else incr better)
            else if p_values.(i) = q_values.(i) then
              incr same
            else
              (if !minimize then incr better else incr worse)
          done ;
          if !worse > 0 then false
          else if !better >0 then true
          else false 
    in

    let dominated_by       = Array.init popsize (fun _ -> []) in
    let dominated_by_count = Array.make popsize (-1) in
    let rank               = Array.make popsize (-1) in

    let front = ref [] in
    let _ =
      Array.iteri (fun pi (p : ('a,'b) Rep.representation) ->
        let count, _ =
          List.fold_left (fun (count,qi) (q : ('a,'b) Rep.representation) ->
            if dominates pi qi then begin          (* > *)
              dominated_by.(ind.(pi)) <- (qi,q) :: dominated_by.(ind.(pi)) ;
              count, qi + 1
            end else if dominates qi pi then begin (* < *)
              count + 1, qi + 1
            end else                               (* = *)
              count, qi + 1
          ) (0,0) pop
        in
        dominated_by_count.(ind.(pi)) <- count ;
        if count = 0 then begin
          front := (pi,p) :: !front ;
          rank.(ind.(pi)) <- 1 ;
        end 
      ) popa
    in
      
    let fronts = ref [] in
    let i = ref 1 in 
    let _ = 
      while (List.length !front) > 0 do
        let set_q_reps = ref [] in 
          
        let _ = 
          debug "multiopt: front i=%d (%d members)\n" !i (List.length !front)
        in
          List.iter (fun (pi,p) -> 
            let s_p = dominated_by.(ind.(pi)) in
              List.iter (fun (qi,q) ->
                let n_q = dominated_by_count.(ind.(qi)) in
                  dominated_by_count.(ind.(qi)) <- (n_q - 1) ;
                  if n_q = 1 then begin
                    rank.(ind.(qi)) <- (!i + 1) ;
                    set_q_reps := (qi,q) :: !set_q_reps 
                  end 
              ) s_p 
          ) !front ;
          incr i ; 
          fronts := !front :: !fronts ;
          front := List.rev !set_q_reps ;
      done 
    in
    let fronts = List.rev !fronts in

    let i_max = !i in 
    let _ = 
      Array.iteri (fun pi p ->
        if rank.(ind.(pi)) = (-1) then begin
          rank.(ind.(pi)) <- i_max ;
          let p_values = fitness.(pi) in 
            if not (is_pessimal p_values) then begin 
              let n_p = dominated_by_count.(ind.(pi)) in
                debug "multiopt: NO RANK for %s %s n_p=%d: setting to %d\n" 
                  names.(pi) (float_array_to_str p_values) n_p i_max
            end 
        end
      ) popa
    in

    (****** 3.3. Crowding Distance ******)
    let distance = Array.create popsize (0.0) in
    let _ = 
      debug "multiopt: crowding distance calculation\n" 
    in
    let _ =
      List.fold_left (fun i front ->
        let front = Array.of_list (List.map fst front) in
          for m = 0 to pred !num_objectives do
            let _ = Array.stable_sort (fun ai bi -> 
              let a_values = fitness.(ai) in
              let b_values = fitness.(bi) in
                compare a_values.(m) b_values.(m) 
            ) front in 
            let i_size = Array.length front in
              assert(i_size > 0); 
              distance.(ind.(front.(0))) <- infinity ;
              distance.(ind.(front.(pred i_size))) <- infinity ;
              for k = 1 to pred (pred i_size) do
                let k_plus_1 = front.(k+1) in 
                let k_minus_1 = front.(k-1) in 
                let k_plus_1_values = fitness.(k_plus_1) in 
                let k_minus_1_values = fitness.(k_minus_1) in 
                let delta =
                    ( 
                      (abs_float (k_plus_1_values.(m) -. k_minus_1_values.(m)))
                      /.
                        (f_max.(m) -. f_min.(m))
                    )
                in
                  distance.(ind.(front.(k))) <- distance.(ind.(front.(k))) +. delta
            done 
          done ;
          i + 1
      ) 1 fronts
    in
      
    (****** 3.4. Selection ******)
    let _ =
      debug "multiopt: computing selection operator\n" 
    in
    let crowded_compare p q = 
      (* "An individual is selected if the rank is lesser than the other or
         if crowding distance is greater than the other" *)
      let ind_pi = Hashtbl.find nametbl (p#name()) in
      let ind_qi = Hashtbl.find nametbl (q#name()) in
      let p_rank = rank.(ind_pi) in
      let q_rank = rank.(ind_qi) in
        if p_rank = q_rank then
          let p_dist = distance.(ind_pi) in
          let q_dist = distance.(ind_qi) in
            compare q_dist p_dist
        else compare p_rank q_rank 
    in 
      crowded_compare, fronts
  end (* end ngsa_ii_sort *)
  in

  let crowded_compare, _ =
    Stats2.time "ngsa_ii_sort" ngsa_ii_sort pop
  in 

  let _ = 
    debug "multiopt: crossover and mutation\n" 
  in

  (* crossover, mutate *) 
  let children = ref [] in 
  let _ = 
    for j = 1 to !Population.popsize do
      let parents = GPPopulation.tournament_selection pop 
        ~compare_func:crowded_compare 2
      in
        match parents with
        | [ one ; two ] ->
          let kids =  GPPopulation.do_cross original one two in 
          let kids = List.map (fun kid -> 
            Search.mutate kid 
          ) kids in 
            children := kids @ !children 
        | _ -> debug "multiopt: wrong number of parents (fatal)\n" 
    done 
  in

  let _ = 
    debug "multiopt: adding children, sorting\n" 
  in

  let many = pop @ !children in 
  let crowded_compare, fronts =
    Stats2.time "ngsa_ii_sort" ngsa_ii_sort many
  in 

    if is_last_generation then begin 
      let f_1 = List.map snd (List.hd fronts) in
      let i = ref 0 in 
      let _ = 
        debug "\nmultiopt: %d in final generation pareto front:\n(does not include all variants considered)\n\n" 
          (List.length f_1) 
      in
      let f_1 = List.sort (fun p q ->
        let p_values = evaluate p in 
        let q_values = evaluate q in 
          compare p_values q_values
      ) f_1 in 
      let copy_and_rename_dir rename_fun src dst =
        let ss = Array.to_list (Sys.readdir src) in
        let ds = List.map rename_fun ss in
        List.iter2 (fun s d ->
          Sys.rename (Filename.concat src s) (Filename.concat dst d)
        ) ss ds
      in
      let _ = 
        let finaldir = Rep.add_subdir (Some "pareto") in
        if not (Sys.file_exists finaldir) then
          Unix.mkdir finaldir 0o755;
        List.iter (fun p ->
          let prefix = Printf.sprintf "pareto-%06d" !i in
          let subdir = Rep.add_subdir (Some prefix) in
          let extant = Sys.file_exists subdir in
          if not extant then
            Unix.mkdir subdir 0o755;
          let p_values = evaluate p in 
          let name = Filename.concat subdir (prefix ^ !Global.extension) in
          let fname = Filename.concat finaldir (prefix ^ ".fitness") in
            incr i; 
            p#output_source name ; 
            if (Sys.file_exists prefix) && (Sys.is_directory prefix) then begin
              copy_and_rename_dir (fun n -> prefix ^ "-" ^ n) prefix finaldir;
              Unix.rmdir prefix
            end;
            let fout = open_out fname in 
              output_string fout (float_array_to_str p_values) ;
              output_string fout "\n" ; 
              close_out fout ; 
              debug "%s %s %s\n" name 
                (float_array_to_str p_values) 
                (p#name ()) 
        ) f_1 
      in 
        f_1 
    end else begin 
      let next_generation, _, _ =
        List.fold_left (fun (gen,fin,i) front ->
          if fin then
            gen, fin, i + 1
          else
            let indivs_in_front = List.map snd front in
            let indivs_in_front =
              let do_not_want = if !minimize then infinity else 0.  in 
                if !no_inf then 
                  List.filter (fun p ->
                    let p_values = evaluate p in 
                      List.for_all (fun v ->
                        v <> do_not_want 
                      ) (Array.to_list p_values) 
                  ) indivs_in_front
                else 
                  indivs_in_front
            in 
            let num_indivs = List.length indivs_in_front in 
            let _ = 
              debug "multiopt: %d individuals in front %d\n" num_indivs i
            in
            let have_sofar = List.length gen in 
            let finished = have_sofar + num_indivs >= !Population.popsize in
            let to_add = 
              if have_sofar + num_indivs <= !Population.popsize then
                (* we can just take them all! *) 
                indivs_in_front 
              else begin
                (* sort by crowding distance *) 
                let sorted = List.sort crowded_compare indivs_in_front in 
                let num_wanted = !Population.popsize - have_sofar in 
                let selected = first_nth sorted num_wanted in 
                  selected 
              end 
            in
            to_add @ gen, finished, i + 1
        ) ([],false,1) fronts
      in
      let next_generation =
        if (List.length next_generation) < !Population.popsize then
          let wanted = !Population.popsize - (List.length next_generation) in 
          let _ =
            debug "multiopt: including %d copies of original\n" wanted 
          in
            List.fold_left (fun gen _ ->
              (original#copy ()) :: gen
            ) next_generation (1 -- wanted)
        else
          next_generation
      in
      let _ = 
        debug "multiopt: next generation has size %d\n" (llen next_generation)
      in
        next_generation 
    end 


